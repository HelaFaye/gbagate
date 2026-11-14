/*
 * mmo_uart_esp32s3.c -- the board's way to the shared overworld's server in
 * the hybrid emulator image (GBAGATE_UART_BRIDGE): mmo_net messages over
 * UART2, which QEMU puts on a TCP socket (qemu/accurate_mmo.py's server
 * connects to it).
 *
 * A stand-in, and said so on the device: a board reaches the server over its
 * own network, which is to come (doc/MMO.md). Only the transport stands in;
 * the messages, and the relay that makes and takes them (src/mmo_relay.c),
 * are the board's own.
 */

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mmo_net.h"

static const char *TAG = "mmo_net";

#define MMO_UART  UART_NUM_2
#ifndef GBAGATE_MMO_TX_GPIO
#define GBAGATE_MMO_TX_GPIO 39
#endif
#ifndef GBAGATE_MMO_RX_GPIO
#define GBAGATE_MMO_RX_GPIO 40
#endif
#define MMO_BAUD  921600

/* gate_app.c: a message from the server, for the relay. */
extern void mmo_from_server(const mn_msg_t *m);

static bool s_up;

static void rx_task(void *arg) {
    (void)arg;
    mn_rx_t rx;
    mn_rx_init(&rx);
    uint8_t buf[64];
    for (;;) {
        int n = uart_read_bytes(MMO_UART, buf, 1, pdMS_TO_TICKS(20));   /* then the rest, at once */
        if (n > 0) {
            size_t more = 0;
            uart_get_buffered_data_len(MMO_UART, &more);
            if (more > sizeof buf - 1) more = sizeof buf - 1;
            if (more) n += uart_read_bytes(MMO_UART, buf + 1, (uint32_t)more, 0);
        }
        for (int i = 0; i < n; i++) {
            mn_msg_t m;
            if (mn_rx_byte(&rx, buf[i], &m)) {
                mmo_from_server(&m);
            }
        }
    }
}

esp_err_t mmo_net_start(void) {
    if (s_up) {
        return ESP_OK;
    }
    uart_config_t cfg = {
        .baud_rate = MMO_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(MMO_UART, 1024, 1024, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(MMO_UART, &cfg);
    if (err == ESP_OK) err = uart_set_pin(MMO_UART, GBAGATE_MMO_TX_GPIO, GBAGATE_MMO_RX_GPIO,
                                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART2 for the server: %s", esp_err_to_name(err));
        return err;
    }
    xTaskCreatePinnedToCore(rx_task, "mmo_net", 3072, NULL, 4, NULL, 0);
    s_up = true;
    ESP_LOGI(TAG, "server tunnel on UART2 (a stand-in for the board's network)");
    return ESP_OK;
}

void mmo_net_send(const mn_msg_t *m) {
    if (!s_up) {
        return;
    }
    uint8_t buf[MN_MAX_FRAMED];
    size_t n = mn_encode(m, buf, sizeof buf);
    if (n) {
        uart_write_bytes(MMO_UART, buf, n);
    }
}
