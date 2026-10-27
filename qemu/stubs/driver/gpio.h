#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)
typedef enum { GPIO_MODE_INPUT, GPIO_MODE_OUTPUT, GPIO_MODE_INPUT_OUTPUT_OD, GPIO_MODE_INPUT_OUTPUT } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE, GPIO_PULLUP_ENABLE } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE, GPIO_PULLDOWN_ENABLE } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE = 0 } gpio_int_type_t;
typedef struct { uint64_t pin_bit_mask; gpio_mode_t mode; gpio_pullup_t pull_up_en; gpio_pulldown_t pull_down_en; gpio_int_type_t intr_type; } gpio_config_t;
static inline esp_err_t gpio_reset_pin(int p) { (void)p; return ESP_OK; }
static inline esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
static inline esp_err_t gpio_set_level(gpio_num_t p, uint32_t l) { (void)p;(void)l; return ESP_OK; }
static inline int gpio_get_level(gpio_num_t p) { (void)p; return 0; }
static inline esp_err_t gpio_set_direction(gpio_num_t p, gpio_mode_t m) { (void)p;(void)m; return ESP_OK; }

/* ISR service, for the infrared driver. */
typedef void (*gpio_isr_t)(void *arg);
#define ESP_INTR_FLAG_IRAM (1<<9)
#define GPIO_INTR_ANYEDGE 3
#define GPIO_INTR_POSEDGE 1
#define GPIO_INTR_NEGEDGE 2
static inline esp_err_t gpio_install_isr_service(int flags) { (void)flags; return ESP_OK; }
static inline esp_err_t gpio_isr_handler_add(gpio_num_t p, gpio_isr_t f, void *a) { (void)p;(void)f;(void)a; return ESP_OK; }
static inline esp_err_t gpio_set_intr_type(gpio_num_t p, int t) { (void)p;(void)t; return ESP_OK; }
