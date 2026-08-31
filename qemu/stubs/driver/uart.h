#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef enum { UART_NUM_0, UART_NUM_1, UART_NUM_2 } uart_port_t;
typedef enum { UART_DATA_8_BITS } uart_word_length_t;
typedef enum { UART_PARITY_DISABLE } uart_parity_t;
typedef enum { UART_STOP_BITS_1 } uart_stop_bits_t;
typedef enum { UART_HW_FLOWCTRL_DISABLE } uart_hw_flowcontrol_t;
typedef enum { UART_SCLK_DEFAULT } uart_sclk_t;
#define UART_PIN_NO_CHANGE (-1)
typedef struct {
    int baud_rate; uart_word_length_t data_bits; uart_parity_t parity;
    uart_stop_bits_t stop_bits; uart_hw_flowcontrol_t flow_ctrl;
    int rx_flow_ctrl_thresh; uart_sclk_t source_clk;
} uart_config_t;
static inline esp_err_t uart_driver_install(uart_port_t p, int rx, int tx, int q, void *h, int f) { (void)p;(void)rx;(void)tx;(void)q;(void)h;(void)f; return ESP_OK; }
static inline bool uart_is_driver_installed(uart_port_t p) { (void)p; return true; }
static inline esp_err_t uart_param_config(uart_port_t p, const uart_config_t *c) { (void)p;(void)c; return ESP_OK; }
static inline esp_err_t uart_set_pin(uart_port_t p, int tx, int rx, int rts, int cts) { (void)p;(void)tx;(void)rx;(void)rts;(void)cts; return ESP_OK; }
static inline int uart_write_bytes(uart_port_t p, const void *b, size_t n) { (void)p;(void)b; return (int)n; }
static inline int uart_read_bytes(uart_port_t p, void *b, uint32_t n, TickType_t t) { (void)p;(void)b;(void)n;(void)t; return 0; }
