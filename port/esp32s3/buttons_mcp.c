/*
 * buttons_mcp.c -- the switches, behind an I2C expander.
 *
 * For boards without pins to spare. A Supermini exposes fourteen usable GPIO;
 * the link port, display and SD card take twelve, which leaves two -- exactly
 * enough for I2C, and not nearly enough for ten switches.
 *
 * Two pins in, sixteen out. The trade is a bus hop on every poll, which at
 * 400 kHz costs about 60 microseconds for a two-byte read, against a UI that
 * polls at 60 Hz. It is not close to mattering.
 *
 * Not yet run on hardware.
 *
 * Wiring
 * ------
 * Every switch shorts its expander pin to ground; the internal pull-ups hold
 * them high at rest. The bit order below is the one ui_input.c expects, so a
 * board should be laid out to match rather than the mapping changed -- there
 * is no reason to make the two disagree.
 */

#include <string.h>

#include "driver/i2c_master.h"
#include "board.h"
#include "esp_log.h"

#include "mcp23017.h"
#include "ui_input.h"

static const char *TAG = "buttons_mcp";

/*
 * A second expander address, so a board can carry both this and a chip reader
 * without them colliding. A2..A0 strapped to 001.
 */
#define BUTTONS_ADDR (MCP_ADDR_BASE + 1)

#define I2C_SDA PIN_I2C_SDA
#define I2C_SCL PIN_I2C_SCL
#define I2C_HZ  400000

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static mcp_t      s_mcp;
static ui_input_t s_input;

static bool xfer(void *ctx, uint8_t addr, const uint8_t *w, size_t wlen,
                 uint8_t *r, size_t rlen) {
    (void)ctx;
    (void)addr;
    if (!s_dev) {
        return false;
    }
    if (rlen == 0) {
        return i2c_master_transmit(s_dev, w, wlen, 50) == ESP_OK;
    }
    return i2c_master_transmit_receive(s_dev, w, wlen, r, rlen, 50) == ESP_OK;
}

/*
 * Map the expander's sixteen pins onto the button bits. GPA0..GPA7 then
 * GPB0..GPB1 covers the ten a five-way-plus-five board needs, in the order
 * ui_input.c already uses -- so the mask needs no rearranging and there is no
 * table here to get out of step with the one in ui_input.h.
 */
static uint32_t read_mapped(void *ctx) {
    uint32_t raw = mcp_buttons_read(ctx);
    /* BTN_UP is bit 0 and GPA0 is bit 0, and so on up to BTN_START at bit 9.
     * The identity mapping is deliberate: a board is easier to lay out to
     * match the firmware than the firmware is to keep in step with a board. */
    return raw & 0x03FF;
}

ui_input_t *input_active(void) { return &s_input; }

esp_err_t buttons_mcp_init(input_mode_t mode) {
    if (mode == INPUT_ENCODER) {
        /* An encoder cannot be sampled through an I2C hop; the caller falls
         * back to the switches on this expander. */
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_bus == NULL) {
        i2c_master_bus_config_t bus = {
            .i2c_port = -1,
            .sda_io_num = I2C_SDA,
            .scl_io_num = I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = { .enable_internal_pullup = true },
        };
        if (i2c_new_master_bus(&bus, &s_bus) != ESP_OK) {
            ESP_LOGE(TAG, "no I2C bus; there will be no input at all");
            return ESP_FAIL;
        }
    }

    if (s_dev == NULL) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = BUTTONS_ADDR,
            .scl_speed_hz    = I2C_HZ,
        };
        if (i2c_master_bus_add_device(s_bus, &dev, &s_dev) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    if (!mcp_buttons_init(&s_mcp, xfer, NULL, BUTTONS_ADDR)) {
        /*
         * Unlike a missing chip reader, this is fatal to using the device:
         * there is no other way to press anything. Say so plainly rather than
         * booting to a menu nobody can move.
         */
        ESP_LOGE(TAG, "no expander at 0x%02X -- the buttons will not work",
                 BUTTONS_ADDR);
        return ESP_FAIL;
    }

    ui_input_init(&s_input, mode, read_mapped, &s_mcp);
    ESP_LOGI(TAG, "%s on an expander at 0x%02X",
             input_mode_name(mode), BUTTONS_ADDR);
    return ESP_OK;
}
