/*
 * i2c_esp32s3.c -- the I2C transfer mcp23017.c is written against.
 *
 * Two pins for the whole chip reader: eleven pad lines and two detect switches
 * live behind the expander. See the pin budget in README.md.
 *
 * Not yet run on hardware.
 */

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "board.h"
#include "esp_log.h"

#include "chip_slot.h"
#include "mcp23017.h"

static const char *TAG = "i2c";

#define I2C_SDA      PIN_I2C_SDA
#define I2C_SCL      PIN_I2C_SCL
#define I2C_HZ       400000
#define I2C_TIMEOUT  50   /* ms; a missing board must fail fast, not stall the UI */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static mcp_t s_mcp;

static bool i2c_xfer(void *ctx, uint8_t addr, const uint8_t *w, size_t wlen,
                     uint8_t *r, size_t rlen) {
    (void)ctx;
    (void)addr;   /* the device handle carries it */
    if (!s_dev) {
        return false;
    }
    esp_err_t err;
    if (rlen == 0) {
        err = i2c_master_transmit(s_dev, w, wlen, I2C_TIMEOUT);
    } else {
        err = i2c_master_transmit_receive(s_dev, w, wlen, r, rlen, I2C_TIMEOUT);
    }
    return err == ESP_OK;
}

/*
 * Called by app_main to decide whether Read Chip appears in the menu. A board
 * that is not fitted is the normal case, not an error, so this reports false
 * quietly rather than logging a failure.
 */
bool chip_reader_probe(void) {
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
            ESP_LOGW(TAG, "no I2C bus; the chip reader cannot be used");
            return false;
        }
    }

    if (s_dev == NULL) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = MCP_ADDR_BASE,
            .scl_speed_hz    = I2C_HZ,
        };
        if (i2c_master_bus_add_device(s_bus, &dev, &s_dev) != ESP_OK) {
            return false;
        }
    }

    if (!mcp_init(&s_mcp, i2c_xfer, NULL, MCP_ADDR_BASE)) {
        return false;
    }
    ESP_LOGI(TAG, "chip reader found at 0x%02X", MCP_ADDR_BASE);
    return true;
}

/* The reader's bus and its instance, for whoever drives the scan. */
const chip_bus_io_t *chip_reader_bus(void) { return mcp_bus_io(); }
void               *chip_reader_ctx(void) { return &s_mcp; }

bool chip_reader_detect(bool *advance, bool *beast) {
    return mcp_read_detect(&s_mcp, advance, beast);
}
