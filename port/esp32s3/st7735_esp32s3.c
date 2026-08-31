/*
 * st7735_esp32s3.c -- ST7735 driver for the ESP32-S3 Supermini.
 *
 * Framebuffer in internal SRAM, DMA flush of dirty spans only, and the list
 * area scrolled by the controller rather than repainted. Geometry and scroll
 * arithmetic live in st7735_panel.c, which is host-tested; this file is the
 * SPI plumbing around it and has not met real hardware.
 *
 * Threading
 * ---------
 * Everything here runs on core 0. The link task owns core 1 and must never
 * wait on this: a full flush is ~13 ms and a missed multiplayer slot is fatal.
 * The queued DMA path leaves the CPU free during transfers, so the two do not
 * contend beyond memory bandwidth.
 *
 * Framebuffer placement
 * ---------------------
 * 128 x 128 x 2 = 32 KB, in internal DMA-capable SRAM, not PSRAM. The S3FH4R2
 * has 2 MB of quad PSRAM and it would fit easily, but PSRAM reads go through
 * the same cache that a flash write suspends -- the exact hazard the link task
 * is built to avoid -- and DMA from PSRAM is slower. 32 KB of internal SRAM is
 * affordable; spend it.
 */

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "st7735_panel.h"
#include "ui_list.h"

static const char *TAG = "st7735";

/* Forward declaration: st7735_set_offset() (below) calls this before its
 * definition further down. Required under -std=gnu23, where an implicit
 * function declaration is a hard error rather than a warning. */
void st7735_mark_all_dirty(void);

/* ---- wiring ----------------------------------------------------------
 *
 * Supermini GPIO is tight, so the display gets four pins and no more:
 * RST ties to the board's EN line, and the backlight is hardwired to 3V3.
 * Leaves the link port, I2C for the chip reader, and the encoder.
 */
#define PIN_SCK 12
#define PIN_SDA 11  /* MOSI; the panel is write-only, MISO is not connected */
#define PIN_CS  10
#define PIN_DC   9

#define SPI_HOST_ID SPI2_HOST

/*
 * 20 MHz. The ST7735 datasheet allows a 66 ns write cycle, so 15 MHz is the
 * in-spec ceiling; 20 works reliably on short traces and buys 25% off the
 * flush. Drop to 15 if you see tearing or dropped columns on a long ribbon.
 */
#define SPI_CLOCK_HZ (20 * 1000 * 1000)

/* Two queued transactions keeps DMA busy while the next span is prepared. */
#define SPI_QUEUE_DEPTH 2

/* ---- ST7735 commands -------------------------------------------------- */

#define CMD_SWRESET 0x01
#define CMD_SLPOUT  0x11
#define CMD_INVOFF  0x20
#define CMD_DISPON  0x29
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_MADCTL  0x36
#define CMD_COLMOD  0x3A
#define CMD_VSCRDEF 0x33
#define CMD_VSCRSADD 0x37

#define MADCTL_RGB  0x00
#define MADCTL_BGR  0x08

#define COLMOD_16BIT 0x05

/* ---- state ------------------------------------------------------------ */

static spi_device_handle_t s_spi;
static st7735_panel_cfg_t  s_panel;
static uint16_t           *s_fb;          /* RGB565, row major */
static bool                s_dirty[UI_SCREEN_H];
static bool                s_any_dirty;
static SemaphoreHandle_t   s_lock;

/*
 * Colour order and whether the panel has been initialised. Kept as state
 * rather than a constant so the Settings screen can correct a module with red
 * and blue swapped, and so the correction survives a power cycle.
 */
static bool                s_bgr = true;
static bool                s_ready;

/* ---- low level -------------------------------------------------------- */

static void st_pre_cb(spi_transaction_t *t) {
    /* user carries the D/C level for this transaction. Set in the pre-callback
     * so it lands exactly at the start of the transfer rather than racing it. */
    gpio_set_level(PIN_DC, (int)(intptr_t)t->user);
}

static esp_err_t st_write(const uint8_t *data, size_t len, bool is_data) {
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

static esp_err_t st_cmd(uint8_t cmd) {
    return st_write(&cmd, 1, false);
}

static esp_err_t st_data(const uint8_t *d, size_t n) {
    return st_write(d, n, true);
}

static esp_err_t st_cmd_args(uint8_t cmd, const uint8_t *args, size_t n) {
    esp_err_t err = st_cmd(cmd);
    if (err == ESP_OK && n) {
        err = st_data(args, n);
    }
    return err;
}

/* ---- address window --------------------------------------------------- */

static esp_err_t st_set_window(uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1) {
    uint8_t buf[4];

    buf[0] = (uint8_t)(x0 >> 8); buf[1] = (uint8_t)x0;
    buf[2] = (uint8_t)(x1 >> 8); buf[3] = (uint8_t)x1;
    esp_err_t err = st_cmd_args(CMD_CASET, buf, 4);
    if (err != ESP_OK) {
        return err;
    }

    buf[0] = (uint8_t)(y0 >> 8); buf[1] = (uint8_t)y0;
    buf[2] = (uint8_t)(y1 >> 8); buf[3] = (uint8_t)y1;
    return st_cmd_args(CMD_RASET, buf, 4);
}

/* ---- init ------------------------------------------------------------- */

static esp_err_t st_panel_init(void) {
    esp_err_t err;

    if ((err = st_cmd(CMD_SWRESET)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(150));

    if ((err = st_cmd(CMD_SLPOUT)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t colmod = COLMOD_16BIT;
    if ((err = st_cmd_args(CMD_COLMOD, &colmod, 1)) != ESP_OK) return err;

    /*
     * MADCTL selects scan order and colour order. BGR is right for most of
     * these modules; a unit with red and blue swapped is corrected from the
     * Settings screen rather than by editing this line, and the choice is
     * stored -- see st7735_set_bgr() below.
     * Leave the row/column swap bits clear -- the scroll region is defined in
     * memory rows, and rotating the scan would put the scrolling axis across
     * the list instead of along it.
     */
    uint8_t madctl = s_bgr ? MADCTL_BGR : MADCTL_RGB;
    if ((err = st_cmd_args(CMD_MADCTL, &madctl, 1)) != ESP_OK) return err;

    if ((err = st_cmd(CMD_INVOFF)) != ESP_OK) return err;
    if ((err = st_cmd(CMD_DISPON)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

esp_err_t st7735_init(st7735_panel_t panel) {
    st7735_panel_init(&s_panel, panel);

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_fb = heap_caps_malloc(UI_FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_fb) {
        ESP_LOGE(TAG, "no room for a %d byte framebuffer", UI_FRAME_BYTES);
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, UI_FRAME_BYTES);
    memset(s_dirty, 0, sizeof s_dirty);

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_DC),
        .mode         = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_SDA,
        .miso_io_num     = -1,      /* write-only panel */
        .sclk_io_num     = PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        /* One full-width row of 12px is the largest span we ever push in one
         * go during scrolling; a full flush is chunked to this. */
        .max_transfer_sz = UI_SCREEN_W * UI_ROW_H * 2,
    };
    if ((err = spi_bus_initialize(SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO)) != ESP_OK) {
        return err;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_CLOCK_HZ,
        .mode           = 0,
        .spics_io_num   = PIN_CS,
        .queue_size     = SPI_QUEUE_DEPTH,
        .pre_cb         = st_pre_cb,
        .flags          = SPI_DEVICE_NO_DUMMY,
    };
    if ((err = spi_bus_add_device(SPI_HOST_ID, &dev, &s_spi)) != ESP_OK) {
        return err;
    }

    if ((err = st_panel_init()) != ESP_OK) {
        return err;
    }

    /* Hand the list area to the controller's scroll engine. */
    if (!st7735_scroll_define(&s_panel, UI_LIST_Y, UI_LIST_H)) {
        ESP_LOGE(TAG, "scroll region rejected");
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t tfa, vsa, bfa;
    st7735_scroll_args(&s_panel, &tfa, &vsa, &bfa);
    uint8_t args[6] = {
        (uint8_t)(tfa >> 8), (uint8_t)tfa,
        (uint8_t)(vsa >> 8), (uint8_t)vsa,
        (uint8_t)(bfa >> 8), (uint8_t)bfa,
    };
    if ((err = st_cmd_args(CMD_VSCRDEF, args, sizeof args)) != ESP_OK) {
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "%ux%u, offset %u,%u, %s, scroll tfa=%u vsa=%u bfa=%u",
             s_panel.width, s_panel.height, s_panel.colstart, s_panel.rowstart,
             s_bgr ? "BGR" : "RGB", tfa, vsa, bfa);
    return ESP_OK;
}

/* Runtime calibration: draw a 1px border and nudge until it closes. */
/*
 * Colour order.
 *
 * This was being called from the settings screen and did not exist -- the
 * hardware image would not have linked. Worth noting how it slipped through:
 * QEMU builds use st7735_qemu.c instead, so the QEMU lint pass never saw the
 * call, and the hardware lint pass did not cover this file. Both now do.
 *
 * Safe before init: the value is applied when MADCTL is next written, and
 * re-sent immediately if the panel is already up, so a change takes effect
 * while the user is looking at it.
 */
void st7735_set_bgr(bool bgr) {
    if (s_bgr == bgr) {
        return;
    }
    s_bgr = bgr;
    if (s_ready) {
        uint8_t madctl = s_bgr ? MADCTL_BGR : MADCTL_RGB;
        st_cmd_args(CMD_MADCTL, &madctl, 1);
    }
}

void st7735_set_offset(uint8_t colstart, uint8_t rowstart) {
    st7735_panel_set_offset(&s_panel, colstart, rowstart);
    st7735_mark_all_dirty();
}

/* ---- framebuffer access ----------------------------------------------- */

uint16_t *st7735_framebuffer(void) { return s_fb; }

void st7735_mark_dirty(uint8_t y, uint8_t h) {
    if (y >= UI_SCREEN_H) {
        return;
    }
    uint16_t end = (uint16_t)y + h;
    if (end > UI_SCREEN_H) {
        end = UI_SCREEN_H;
    }
    for (uint16_t r = y; r < end; r++) {
        s_dirty[r] = true;
    }
    s_any_dirty = true;
}

void st7735_mark_all_dirty(void) {
    memset(s_dirty, 1, sizeof s_dirty);
    s_any_dirty = true;
}

/* ---- flush ------------------------------------------------------------ */

/*
 * Push a run of framebuffer rows. Rows are contiguous in the framebuffer, so a
 * whole span goes out as one DMA transfer -- which is the entire reason for
 * tracking dirt by row rather than by rectangle.
 */
static esp_err_t st_flush_span(uint8_t y, uint8_t rows) {
    uint16_t x0, x1, y0, y1;
    if (!st7735_window(&s_panel, 0, y, UI_SCREEN_W, rows, &x0, &x1, &y0, &y1)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = st_set_window(x0, x1, y0, y1);
    if (err != ESP_OK) {
        return err;
    }
    if ((err = st_cmd(CMD_RAMWR)) != ESP_OK) {
        return err;
    }
    return st_data((const uint8_t *)(s_fb + (size_t)y * UI_SCREEN_W),
                   (size_t)rows * UI_SCREEN_W * 2);
}

/*
 * Flush every dirty row, coalescing adjacent ones into single transfers.
 * Returns bytes actually sent, which is worth logging during bring-up: if a
 * simple cursor move is pushing 24 KB, something is over-marking.
 */
size_t st7735_flush(void) {
    if (!s_any_dirty) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    size_t sent = 0;
    uint16_t r = 0;
    while (r < UI_SCREEN_H) {
        if (!s_dirty[r]) {
            r++;
            continue;
        }
        uint16_t start = r;
        /* Chunk to max_transfer_sz so one big span cannot exceed the DMA
         * descriptor limit configured on the bus. */
        while (r < UI_SCREEN_H && s_dirty[r] && (r - start) < UI_ROW_H) {
            r++;
        }
        uint8_t rows = (uint8_t)(r - start);
        if (st_flush_span((uint8_t)start, rows) == ESP_OK) {
            sent += (size_t)rows * UI_SCREEN_W * 2;
            memset(&s_dirty[start], 0, rows);
        } else {
            ESP_LOGW(TAG, "flush failed at row %u", start);
            break;
        }
    }

    s_any_dirty = false;
    for (uint16_t i = 0; i < UI_SCREEN_H; i++) {
        if (s_dirty[i]) {
            s_any_dirty = true;
            break;
        }
    }

    xSemaphoreGive(s_lock);
    return sent;
}

/* ---- hardware scroll -------------------------------------------------- */

/*
 * Scroll the list by whole rows and repaint only the strip that wrapped.
 *
 * delta_rows is in list rows, positive meaning the list advances. The caller
 * must already have rendered the new content into the framebuffer rows that
 * st7735_scroll_render_rows() reports.
 *
 * One row costs 128 x 12 x 2 = 3072 bytes, against 24576 for repainting the
 * whole list. That eight-to-one is what keeps a fast encoder spin smooth.
 */
esp_err_t st7735_scroll_rows(int32_t delta_rows, uint8_t fb_row_y) {
    if (delta_rows == 0) {
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    int32_t delta_px = delta_rows * (int32_t)UI_ROW_H;
    uint16_t mem_row = st7735_scroll_fill_row(&s_panel, delta_px);

    /*
     * The strip can straddle the end of the scroll region, in which case it is
     * written in two pieces: the part before the wrap, then the remainder at
     * the top of the region.
     */
    uint16_t y0, y1;
    uint8_t before_wrap;
    st7735_window_raw(&s_panel, mem_row, UI_ROW_H, &y0, &y1, &before_wrap);

    esp_err_t err = ESP_OK;
    const uint8_t *src = (const uint8_t *)(s_fb + (size_t)fb_row_y * UI_SCREEN_W);
    uint16_t cx0 = s_panel.colstart;
    uint16_t cx1 = (uint16_t)(s_panel.colstart + UI_SCREEN_W - 1);

    if (before_wrap) {
        err = st_set_window(cx0, cx1, y0, y1);
        if (err == ESP_OK) err = st_cmd(CMD_RAMWR);
        if (err == ESP_OK) err = st_data(src, (size_t)before_wrap * UI_SCREEN_W * 2);
        src += (size_t)before_wrap * UI_SCREEN_W * 2;
    }

    uint8_t remainder = (uint8_t)(UI_ROW_H - before_wrap);
    if (err == ESP_OK && remainder) {
        uint16_t top = s_panel.tfa;
        err = st_set_window(cx0, cx1, top, (uint16_t)(top + remainder - 1));
        if (err == ESP_OK) err = st_cmd(CMD_RAMWR);
        if (err == ESP_OK) err = st_data(src, (size_t)remainder * UI_SCREEN_W * 2);
    }

    if (err == ESP_OK) {
        uint16_t addr = st7735_scroll_by(&s_panel, delta_px);
        uint8_t args[2] = { (uint8_t)(addr >> 8), (uint8_t)addr };
        err = st_cmd_args(CMD_VSCRSADD, args, sizeof args);
    }

    xSemaphoreGive(s_lock);
    return err;
}

/*
 * Where in the framebuffer the caller should render the row that is about to
 * be revealed. Kept alongside the scroll call so the two cannot disagree about
 * which row is being staged.
 */
uint8_t st7735_scroll_render_rows(int32_t delta_rows) {
    /* The framebuffer is a flat 128x128 image; the row being revealed is the
     * one at the far edge of the list area in the direction of travel. */
    if (delta_rows > 0) {
        return (uint8_t)(UI_LIST_Y + UI_LIST_H - UI_ROW_H);
    }
    return (uint8_t)UI_LIST_Y;
}

/* Put the scroll engine back to zero, e.g. when switching library tabs. */
esp_err_t st7735_scroll_reset(void) {
    uint16_t addr = st7735_scroll_set(&s_panel, 0);
    uint8_t args[2] = { (uint8_t)(addr >> 8), (uint8_t)addr };
    esp_err_t err = st_cmd_args(CMD_VSCRSADD, args, sizeof args);
    st7735_mark_all_dirty();
    return err;
}
