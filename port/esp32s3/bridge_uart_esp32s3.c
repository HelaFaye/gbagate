/*
 * bridge_uart_esp32s3.c -- GB-Link's bridge UART on the ESP32-S3.
 *
 * GB-Link 2.2.7 carries its 'GB' frames (| 0x47 0x42 | channel | len LE |
 * payload |) on a second serial port, GP8/GP9 at 921600 baud, to a companion
 * MCU; GB-Link-Switch-LDN is that companion for the Wireless Adapter mode,
 * bridging a GBA to a Switch. This is that port here: UART2 on PIN_BRIDGE_TX /
 * PIN_BRIDGE_RX (board.h), feeding the vendored SerialLayer's bridge instance
 * (port/gblink/serial_layer.cpp) from a task, never from the UART interrupt,
 * since a frame can enter a whole link mode.
 *
 * Built in only with BOARD_HAS_BRIDGE.
 *
 * NOT YET RUN ON HARDWARE.
 */
#include "board.h"

#if BOARD_HAS_BRIDGE

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gblink_port.h"

static const char *TAG = "bridge";

#define BRIDGE_UART  UART_NUM_2
#define BRIDGE_BAUD  921600
#define BRIDGE_BUF   4096

static void rx_task(void *arg) {
    (void)arg;
    static uint8_t buf[256];
    for (;;) {
        const int n = uart_read_bytes(BRIDGE_UART, buf, sizeof buf, pdMS_TO_TICKS(10));
        if (n > 0) gblink_bridge_on_receive(buf, (size_t)n);
    }
}

bool gblink_bridge_write(const uint8_t *data, size_t len) {
    return uart_write_bytes(BRIDGE_UART, data, len) == (int)len;
}

void bridge_uart_start(void) {
    const uart_config_t cfg = {
        .baud_rate = BRIDGE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(BRIDGE_UART, BRIDGE_BUF, BRIDGE_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART, PIN_BRIDGE_TX, PIN_BRIDGE_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_pullup_en(PIN_BRIDGE_RX);   /* an idle UART line is high */
    xTaskCreatePinnedToCore(rx_task, "bridge_rx", 3072, NULL, 10, NULL, 0);
    ESP_LOGI(TAG, "GB-Link bridge UART: TX=%d RX=%d at %d", PIN_BRIDGE_TX, PIN_BRIDGE_RX, BRIDGE_BAUD);
}

#else

void bridge_uart_start(void) {}

#endif
