/*
 * transport_uart.c -- link_bridge over a plain UART.
 *
 * This exists for QEMU. Espressif's QEMU does not emulate the ESP32-S3 USB-OTG
 * peripheral, so the CDC transport never enumerates there and cannot be the
 * channel to a host. UART, however, QEMU maps straight onto a socket -- which
 * makes it the one way to talk to firmware running under emulation.
 *
 * It is the same bridge either way: link_bridge already takes the transport as
 * a callback and already frames byte streams with SLIP, so this file is thin
 * and the protocol under test is the real one, not a QEMU-shaped substitute.
 *
 * On real hardware use transport_esp32s3.c instead; UART0 is the console there.
 */

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "link_bridge.h"

static const char *TAG = "uart_tp";

/*
 * UART1, not UART0. Under QEMU, UART0 carries the ESP-IDF console: sharing it
 * would interleave log lines with SLIP frames and corrupt both. The run script
 * gives UART1 its own socket.
 */
#define TP_UART      UART_NUM_1
#define TP_TX_PIN    17
#define TP_RX_PIN    18
#define TP_BAUD      921600
#define TP_BUF       2048

static link_bridge_t *s_bridge;

static uint32_t now_us(void) {
    return (uint32_t)esp_timer_get_time();
}

static bool uart_tx(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    int written = uart_write_bytes(TP_UART, (const char *)data, len);
    return written == (int)len;
}

/*
 * Reads on core 0. The link task must never wait on this: a host that stops
 * reading is normal, a stalled transfer is not.
 */
static void uart_rx_task(void *arg) {
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        int n = uart_read_bytes(TP_UART, buf, sizeof buf, pdMS_TO_TICKS(20));
        if (n > 0) {
            bridge_feed(s_bridge, buf, (size_t)n, now_us());
        }
        bridge_tick(s_bridge, now_us());
    }
}

esp_err_t transport_uart_start(link_bridge_t *bridge) {
    s_bridge = bridge;
    bridge_init(bridge, uart_tx, NULL, /* stream = */ true, GBA_ROLE_MASTER);

    const uart_config_t cfg = {
        .baud_rate = TP_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(TP_UART, TP_BUF, TP_BUF, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    if ((err = uart_param_config(TP_UART, &cfg)) != ESP_OK) {
        return err;
    }
    if ((err = uart_set_pin(TP_UART, TP_TX_PIN, TP_RX_PIN,
                            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)) != ESP_OK) {
        return err;
    }

    xTaskCreatePinnedToCore(uart_rx_task, "uart_tp", 4096, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "UART transport up on UART%d at %d baud", TP_UART, TP_BAUD);
    return ESP_OK;
}
