/*
 * mb_spi_esp32s3.c -- Normal 32-bit SIO master, for gba_multiboot.
 *
 * During multiboot the adapter drives the clock, which makes this the least
 * demanding of the link modes: if the CPU stalls, the clock simply stretches
 * and the GBA waits. That is the opposite of the Multi-Play PHY, and it is why
 * this one can use the ordinary SPI driver instead of a pinned bit-bang loop.
 *
 * Not yet run on hardware.
 */

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "board.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "gba_multiboot.h"

static const char *TAG = "mb_spi";

/*
 * Link port, Normal 32-bit mode:
 *   pin 2 SO -> our MISO   (GBA drives, we read)
 *   pin 3 SI -> our MOSI   (we drive, GBA reads)
 *   pin 5 SC -> our SCLK   (we drive)
 *   pin 4 SD               unused in Normal mode
 *   pin 6 GND
 * Pin 1 VCC stays disconnected; the board has its own supply.
 */
#define PIN_MISO PIN_LINK_SI   /* GBA SO */
#define PIN_MOSI PIN_LINK_SD   /* GBA SI */
#define PIN_SCLK PIN_LINK_SC   /* GBA SC */

#define MB_HOST SPI3_HOST

/*
 * The BIOS download routine runs at 115200 during the handshake. The protocol
 * allows shifting to 2 MHz once contact is made, but GBATEK notes 2 MHz is
 * intended only for hardware attached directly to the port with no cable in
 * between -- so a cable-connected adapter should stay slow until it has been
 * measured on a scope.
 */
#define MB_CLOCK_HZ 115200

static spi_device_handle_t s_spi;

/*
 * SPI mode 3: clock idles high, data changes on the falling edge, sampled on
 * the rising edge -- which is what GBATEK describes for Normal mode ("when SC
 * goes LOW each side outputs a bit; when SC goes HIGH each side reads one").
 *
 * VERIFY ON SCOPE: bit order. This assumes MSB first, matching the reference
 * implementations, but a wrong guess here fails as a sync that never completes
 * rather than as anything diagnostic. If sync never lands, try the swap below
 * before suspecting the constants.
 */
static uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

static uint32_t mb_spi_xfer(void *ctx, uint32_t word) {
    (void)ctx;

    uint32_t tx = bswap32(word);
    uint32_t rx = 0;

    spi_transaction_t t;
    memset(&t, 0, sizeof t);
    t.length    = 32;
    t.tx_buffer = &tx;
    t.rx_buffer = &rx;

    if (spi_device_polling_transmit(s_spi, &t) != ESP_OK) {
        return 0xFFFFFFFFu;
    }

    /*
     * The BIOS needs a gap between words. Without it the sync appears to work
     * and the data phase drifts, which is a miserable thing to debug.
     */
    esp_rom_delay_us(36);
    return bswap32(rx);
}

esp_err_t mb_spi_init(void) {
    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = PIN_MISO,
        .sclk_io_num     = PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(MB_HOST, &bus, SPI_DMA_DISABLED),
                        TAG, "bus init");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = MB_CLOCK_HZ,
        .mode           = 3,
        .spics_io_num   = -1,   /* the link port has no chip select */
        .queue_size     = 1,
        .flags          = 0,   /* full duplex: the GBA replies while we send */
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(MB_HOST, &dev, &s_spi), TAG, "add dev");

    ESP_LOGI(TAG, "multiboot SPI ready at %d Hz, mode 3", MB_CLOCK_HZ);
    return ESP_OK;
}

void mb_spi_deinit(void) {
    if (s_spi) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        spi_bus_free(MB_HOST);
    }
}

/* Hand this to mb_init(). */
mb_xfer_fn mb_spi_transfer(void) {
    return mb_spi_xfer;
}
