/*
 * lcd_ili9341_esp32s3.c -- the touch carrier's 320x240 panel (ILI9341, or
 * ST7789 with -DBOARD_LCD_ST7789=1), behind the same st7735_* calls the
 * rest of the firmware draws through.
 *
 * The UI stays 128x128 (ui_list.h). This scales it by 15/8 into the 240x240
 * square in the middle of the panel and draws the touch keys in the strips
 * either side (src/touch_pad.c has the layout and the arithmetic, host
 * tested). So nothing above this file knows the panel is larger.
 *
 * The SPI bus is the board's one: the panel, the SD card and the XPT2046
 * share SCK and MOSI, and the card and the touch controller read on MISO.
 * The bus is set up here, with MISO, because the panel comes up first;
 * sd_esp32s3.c and touch_xpt2046_esp32s3.c add their devices to it.
 *
 * Bytes on the wire: the framebuffer is native uint16 RGB565 (little endian
 * on the S3) and the panel takes the high byte first, so the scaler swaps
 * as it copies -- it touches every pixel anyway.
 *
 * Checked on qemu_esp32 (qemu/accurate.sh boot gate carrier-touch): the
 * panel comes up, the screenshot shows the UI scaled and the keys, and a
 * touch on a key moves the UI. Not yet on a real panel.
 */

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board.h"
#include "st7735_panel.h"
#include "touch_pad.h"
#include "ui_list.h"

#if !BOARD_HAS_TOUCH
#error "lcd_ili9341_esp32s3.c is the touch carrier's panel"
#endif

static const char *TAG = "lcd";

#ifndef BOARD_LCD_ST7789
#define BOARD_LCD_ST7789 0
#endif

#define SPI_HOST_ID   SPI2_HOST
/* 40 MHz: the ILI9341's write cycle is 66 ns (15 MHz) in the datasheet and
 * every module in the wild runs at 40; the ST7789 is rated for 62.5. A full
 * repaint of the square is 115 KB, 23 ms. */
#define SPI_CLOCK_HZ  (40 * 1000 * 1000)

/* One chunk: 16 UI rows scaled, 30 panel rows of 240 pixels: 14 KB. */
#define CHUNK_UI_ROWS 16
#define CHUNK_ROWS_MAX ((CHUNK_UI_ROWS * 15 + 7) / 8 + 1)

#define CMD_SWRESET 0x01
#define CMD_SLPOUT  0x11
#define CMD_INVOFF  0x20
#define CMD_INVON   0x21
#define CMD_DISPON  0x29
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_MADCTL  0x36
#define CMD_COLMOD  0x3A

#define MADCTL_MY   0x80
#define MADCTL_MX   0x40
#define MADCTL_MV   0x20
#define MADCTL_BGR  0x08

static spi_device_handle_t s_spi;
static uint16_t           *s_fb;          /* 128 x 128, what the UI draws   */
static uint16_t           *s_chunk;       /* scaled rows, wire byte order  */
static uint16_t           *s_strip;       /* one key strip                 */
static bool                s_dirty[UI_SCREEN_H];
static bool                s_any_dirty;
static SemaphoreHandle_t   s_lock;
static bool                s_bgr = !BOARD_LCD_ST7789;
static bool                s_ready;
static uint32_t            s_held;        /* keys drawn pressed            */
static uint32_t            s_held_drawn = UINT32_MAX;

void st7735_mark_all_dirty(void);

/* ---- low level ---------------------------------------------------------- */

static void pre_cb(spi_transaction_t *t) {
    gpio_set_level(PIN_LCD_DC, (int)(intptr_t)t->user);
}

static esp_err_t lcd_write(const void *data, size_t len, bool is_data) {
    if (len == 0) {
        return ESP_OK;
    }
    spi_transaction_t t;
    memset(&t, 0, sizeof t);
    t.length    = len * 8;
    t.tx_buffer = data;
    t.user      = (void *)(intptr_t)(is_data ? 1 : 0);
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t lcd_cmd(uint8_t cmd, const uint8_t *args, size_t n) {
    esp_err_t err = lcd_write(&cmd, 1, false);
    if (err == ESP_OK && n) {
        err = lcd_write(args, n, true);
    }
    return err;
}

static esp_err_t lcd_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t c[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t r[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    esp_err_t err = lcd_cmd(CMD_CASET, c, 4);
    if (err == ESP_OK) {
        err = lcd_cmd(CMD_RASET, r, 4);
    }
    if (err == ESP_OK) {
        err = lcd_cmd(CMD_RAMWR, NULL, 0);
    }
    return err;
}

/* Landscape, 320 across: rows and columns exchanged. */
static uint8_t madctl(void) {
    return MADCTL_MV | (s_bgr ? MADCTL_BGR : 0);
}

/* ---- init --------------------------------------------------------------- */

typedef struct { uint8_t cmd, n; uint8_t arg[5]; } init_t;

/* The ILI9341's power and timing set-up, as every driver for these modules
 * sends it (the reset values work, with a paler picture). */
static const init_t k_ili9341[] = {
    { 0xCF, 3, { 0x00, 0xC1, 0x30 } },           /* power control B          */
    { 0xED, 4, { 0x64, 0x03, 0x12, 0x81 } },     /* power-on sequence        */
    { 0xE8, 3, { 0x85, 0x00, 0x78 } },           /* driver timing A          */
    { 0xCB, 5, { 0x39, 0x2C, 0x00, 0x34, 0x02 } },/* power control A         */
    { 0xF7, 1, { 0x20 } },                       /* pump ratio               */
    { 0xEA, 2, { 0x00, 0x00 } },                 /* driver timing B          */
    { 0xC0, 1, { 0x23 } },                       /* power control 1: 4.6 V   */
    { 0xC1, 1, { 0x10 } },                       /* power control 2          */
    { 0xC5, 2, { 0x3E, 0x28 } },                 /* VCOM 1                   */
    { 0xC7, 1, { 0x86 } },                       /* VCOM 2                   */
    { 0xB1, 2, { 0x00, 0x18 } },                 /* frame rate: 79 Hz        */
    { 0xB6, 3, { 0x08, 0x82, 0x27 } },           /* display function         */
};

static esp_err_t panel_init(void) {
    esp_err_t err;
#if PIN_LCD_RST >= 0
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
#endif
    if ((err = lcd_cmd(CMD_SWRESET, NULL, 0)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(150));
#if !BOARD_LCD_ST7789
    for (size_t i = 0; i < sizeof k_ili9341 / sizeof k_ili9341[0]; i++) {
        if ((err = lcd_cmd(k_ili9341[i].cmd, k_ili9341[i].arg, k_ili9341[i].n)) != ESP_OK) {
            return err;
        }
    }
#endif
    uint8_t m = madctl(), colmod = 0x55;         /* 16 bits per pixel        */
    if ((err = lcd_cmd(CMD_MADCTL, &m, 1)) != ESP_OK) return err;
    if ((err = lcd_cmd(CMD_COLMOD, &colmod, 1)) != ESP_OK) return err;
    /* The ST7789's glass is inverted at reset on these modules. */
    if ((err = lcd_cmd(BOARD_LCD_ST7789 ? CMD_INVON : CMD_INVOFF, NULL, 0)) != ESP_OK) return err;
    if ((err = lcd_cmd(CMD_SLPOUT, NULL, 0)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));
    return lcd_cmd(CMD_DISPON, NULL, 0);
}

static esp_err_t draw_strips(void) {
    for (int side = 0; side < 2; side++) {
        tp_draw_strip(s_strip, (tp_side_t)side, s_held);
        for (size_t i = 0; i < TP_STRIP_W * TP_PANEL_H; i++) {
            s_strip[i] = __builtin_bswap16(s_strip[i]);
        }
        uint16_t x0 = side == TP_LEFT ? 0 : TP_RIGHT_X;
        esp_err_t err = lcd_window(x0, 0, x0 + TP_STRIP_W - 1, TP_PANEL_H - 1);
        if (err == ESP_OK) {
            err = lcd_write(s_strip, TP_STRIP_W * TP_PANEL_H * 2, true);
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    s_held_drawn = s_held;
    return ESP_OK;
}

esp_err_t st7735_init(st7735_panel_t panel) {
    (void)panel;                     /* one panel on this board */
    s_lock = xSemaphoreCreateMutex();
    s_fb = heap_caps_calloc(1, UI_FRAME_BYTES, MALLOC_CAP_INTERNAL);
    s_chunk = heap_caps_malloc(CHUNK_ROWS_MAX * TP_UI_SIZE * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_strip = heap_caps_malloc(TP_STRIP_W * TP_PANEL_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_lock || !s_fb || !s_chunk || !s_strip) {
        ESP_LOGE(TAG, "no memory for the framebuffer and line buffers");
        return ESP_ERR_NO_MEM;
    }

    uint64_t outs = (1ULL << PIN_LCD_DC) | (1ULL << PIN_LCD_BL);
#if PIN_LCD_RST >= 0
    outs |= 1ULL << PIN_LCD_RST;
#endif
    gpio_config_t io = { .pin_bit_mask = outs, .mode = GPIO_MODE_OUTPUT };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(PIN_LCD_BL, 0);   /* dark until there is a picture */
#if PIN_LCD_RST >= 0
    gpio_set_level(PIN_LCD_RST, 1);
#endif

    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_SPI_MOSI,
        .miso_io_num     = PIN_SPI_MISO,   /* the card and the touch read on it */
        .sclk_io_num     = PIN_SPI_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = TP_STRIP_W * TP_PANEL_H * 2,   /* the largest: a strip */
    };
    if ((err = spi_bus_initialize(SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO)) != ESP_OK) {
        return err;
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_CLOCK_HZ,
        .mode           = 0,
        .spics_io_num   = PIN_LCD_CS,
        .queue_size     = 2,
        .pre_cb         = pre_cb,
        .flags          = SPI_DEVICE_NO_DUMMY,
    };
    if ((err = spi_bus_add_device(SPI_HOST_ID, &dev, &s_spi)) != ESP_OK) {
        return err;
    }
    if ((err = panel_init()) != ESP_OK) {
        return err;
    }
    if ((err = draw_strips()) != ESP_OK) {
        return err;
    }
    st7735_mark_all_dirty();
    s_ready = true;
    ESP_LOGI(TAG, "%s %ux%u landscape: the UI at 15/8 in %u-%u, touch keys either side",
             BOARD_LCD_ST7789 ? "ST7789" : "ILI9341", TP_PANEL_W, TP_PANEL_H,
             TP_UI_X, TP_RIGHT_X - 1);
    return ESP_OK;
}

/* ---- the st7735_* calls the firmware draws through ----------------------- */

uint16_t *st7735_framebuffer(void) { return s_fb; }

void st7735_mark_dirty(uint8_t y, uint8_t h) {
    for (unsigned r = y; r < (unsigned)y + h && r < UI_SCREEN_H; r++) {
        s_dirty[r] = true;
    }
    s_any_dirty = true;
}

void st7735_mark_all_dirty(void) {
    memset(s_dirty, 1, sizeof s_dirty);
    s_any_dirty = true;
}

void st7735_set_bgr(bool bgr) {
    if (s_bgr == bgr) {
        return;
    }
    s_bgr = bgr;
    if (s_ready) {
        uint8_t m = madctl();
        lcd_cmd(CMD_MADCTL, &m, 1);
        s_held_drawn = UINT32_MAX;   /* repaint the strips too */
        st7735_mark_all_dirty();
    }
}

/* The ST7735's glass offsets; this panel has none. */
void st7735_set_offset(uint8_t colstart, uint8_t rowstart) {
    (void)colstart;
    (void)rowstart;
}

/* UI rows y0..y1-1 scaled into the chunk, then out. */
static esp_err_t flush_span(int y0, int y1) {
    int p0 = tp_dst(y0), p1 = tp_dst(y1);
    uint16_t *out = s_chunk;
    for (int p = p0; p < p1; p++) {
        const uint16_t *src = s_fb + (size_t)tp_src(p) * UI_SCREEN_W;
        for (int x = 0; x < TP_UI_SIZE; x++) {
            *out++ = __builtin_bswap16(src[tp_src(x)]);
        }
    }
    esp_err_t err = lcd_window(TP_UI_X, p0, TP_UI_X + TP_UI_SIZE - 1, p1 - 1);
    if (err == ESP_OK) {
        err = lcd_write(s_chunk, (size_t)(p1 - p0) * TP_UI_SIZE * 2, true);
    }
    return err;
}

size_t st7735_flush(void) {
    if (!s_ready) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t sent = 0;
    if (s_held != s_held_drawn && draw_strips() == ESP_OK) {
        sent += 2 * TP_STRIP_W * TP_PANEL_H * 2;
    }
    if (s_any_dirty) {
        int r = 0;
        while (r < UI_SCREEN_H) {
            if (!s_dirty[r]) {
                r++;
                continue;
            }
            int start = r;
            while (r < UI_SCREEN_H && s_dirty[r] && r - start < CHUNK_UI_ROWS) {
                r++;
            }
            if (flush_span(start, r) != ESP_OK) {
                ESP_LOGW(TAG, "flush failed at row %d", start);
                break;
            }
            sent += (size_t)(tp_dst(r) - tp_dst(start)) * TP_UI_SIZE * 2;
            memset(&s_dirty[start], 0, (size_t)(r - start));
        }
        s_any_dirty = false;
        for (int i = 0; i < UI_SCREEN_H; i++) {
            s_any_dirty |= s_dirty[i];
        }
        gpio_set_level(PIN_LCD_BL, 1);   /* a picture: light it */
    }
    xSemaphoreGive(s_lock);
    return sent;
}

/* The touch driver says which keys are under a finger; they are drawn
 * pressed on the next flush. */
void lcd_touch_held(uint32_t buttons) {
    s_held = buttons;
}

/*
 * The ST7735 scrolls the list in hardware; this panel repaints. The calls
 * are kept so the callers need no second path: a scroll is a repaint of the
 * list rows.
 */
esp_err_t st7735_scroll_rows(int32_t delta_rows, uint8_t fb_row_y) {
    (void)delta_rows;
    (void)fb_row_y;
    st7735_mark_dirty(UI_LIST_Y, UI_LIST_H);
    return ESP_OK;
}

uint8_t st7735_scroll_render_rows(int32_t delta_rows) {
    (void)delta_rows;
    return UI_LIST_Y;
}

esp_err_t st7735_scroll_reset(void) {
    st7735_mark_all_dirty();
    return ESP_OK;
}
