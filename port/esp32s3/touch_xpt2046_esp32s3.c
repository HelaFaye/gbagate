/*
 * touch_xpt2046_esp32s3.c -- the touch carrier's XPT2046, as the buttons.
 *
 * The board has no switches: the touch keys either side of the panel are
 * its buttons (src/touch_pad.h). This file stands where buttons_esp32s3.c
 * stands on the other boards -- the same buttons_init(), input_active() and
 * read function -- so ui_input.c debounces, repeats and long-presses a
 * touch exactly as it does a switch, and input_esp32s3.c posts the events.
 * The mode is always INPUT_DPAD5: all ten buttons have a key.
 *
 * Reading: PENIRQ (PIN_TOUCH_IRQ) is low while the screen is touched, so
 * the bus is used only then. One transaction converts X and Y, the second
 * control byte overlapping the first result, as the datasheet allows (16
 * clocks per conversion); two in a row must agree, or the touch is still
 * settling (or lifting) and is skipped. The last control byte leaves the
 * converter powered down with PENIRQ enabled (PD = 00).
 *
 * The SPI bus is the panel's (lcd_ili9341_esp32s3.c sets it up with MISO);
 * the XPT2046 is rated to 2.5 MHz.
 */

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#include "board.h"
#include "touch_pad.h"
#include "ui_input.h"

#if !BOARD_HAS_TOUCH
#error "touch_xpt2046_esp32s3.c is for a board with a touch panel"
#endif

static const char *TAG = "touch";

#define SPI_HOST_ID  SPI2_HOST
#define SPI_CLOCK_HZ (2 * 1000 * 1000)
#define CMD_X        0xD0     /* start, channel X+, 12-bit, differential, PD 00 */
#define CMD_Y        0x90     /* channel Y+                                     */
#define AGREE        48       /* raw counts two samples may differ by           */

static spi_device_handle_t s_dev;
static ui_input_t          s_input;
static tp_cal_t            s_cal;
static bool                s_up;
static uint32_t            s_touches;      /* for the diagnostics            */
static int                 s_last_x = -1, s_last_y = -1;

extern void lcd_touch_held(uint32_t buttons);

ui_input_t *input_active(void) { return &s_input; }

static bool sample(uint16_t *x, uint16_t *y) {
    uint8_t tx[5] = { CMD_X, 0, CMD_Y, 0, 0 }, rx[5] = { 0 };
    spi_transaction_t t;
    memset(&t, 0, sizeof t);
    t.length = sizeof tx * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    if (spi_device_polling_transmit(s_dev, &t) != ESP_OK) {
        return false;
    }
    *x = (uint16_t)(((rx[1] << 8) | rx[2]) >> 3) & 0x0FFF;
    *y = (uint16_t)(((rx[3] << 8) | rx[4]) >> 3) & 0x0FFF;
    return true;
}

static bool touched(void) { return gpio_get_level(PIN_TOUCH_IRQ) == 0; }

/* The ui_input read: the button under the finger, or none. */
static uint32_t touch_read(void *ctx) {
    (void)ctx;
    uint32_t b = 0;
    uint16_t x1, y1, x2, y2;
    if (s_up && touched() && sample(&x1, &y1) && sample(&x2, &y2) && touched() &&
        abs(x1 - x2) <= AGREE && abs(y1 - y2) <= AGREE &&
        (x1 > TP_RAW_FLOOR || y1 > TP_RAW_FLOOR)) {
        int px, py;
        if (tp_raw_to_panel(&s_cal, (uint16_t)((x1 + x2) / 2), (uint16_t)((y1 + y2) / 2), &px, &py)) {
            b = tp_button_at(px, py);
            if (px != s_last_x || py != s_last_y) {
                s_last_x = px;
                s_last_y = py;
                s_touches++;
            }
        }
    }
    lcd_touch_held(b);
    return b;
}

esp_err_t buttons_init(input_mode_t mode) {
    (void)mode;               /* a touch panel has every button */
    if (!s_dev) {
        gpio_config_t irq = {
            .pin_bit_mask = 1ULL << PIN_TOUCH_IRQ,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,   /* PENIRQ is open drain */
        };
        esp_err_t err = gpio_config(&irq);
        if (err != ESP_OK) {
            return err;
        }
        spi_device_interface_config_t dev = {
            .clock_speed_hz = SPI_CLOCK_HZ,
            .mode           = 0,
            .spics_io_num   = PIN_TOUCH_CS,
            .queue_size     = 1,
        };
        if ((err = spi_bus_add_device(SPI_HOST_ID, &dev, &s_dev)) != ESP_OK) {
            ESP_LOGE(TAG, "no SPI device (%s): is the panel up?", esp_err_to_name(err));
            return err;
        }
        tp_cal_default(&s_cal);
        /* One conversion with PD = 00, so PENIRQ is enabled whatever state
         * the controller came up in. */
        uint16_t x, y;
        sample(&x, &y);
        s_up = true;
    }
    ui_input_init(&s_input, INPUT_DPAD5, touch_read, NULL);
    ESP_LOGI(TAG, "XPT2046 on CS=%d IRQ=%d: ten touch keys", PIN_TOUCH_CS, PIN_TOUCH_IRQ);
    return ESP_OK;
}

bool buttons_present(void) { return s_up; }

uint32_t touch_count(void) { return s_touches; }
