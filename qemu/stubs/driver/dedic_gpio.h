#pragma once
/*
 * Dedicated GPIO: the S3's low-latency bundle interface, which the Multi-Play
 * PHY uses to read and write link pins in a couple of cycles rather than going
 * through the GPIO matrix.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct dedic_gpio_bundle_t *dedic_gpio_bundle_handle_t;
typedef struct {
    const int *gpio_array;
    size_t array_size;
    struct { unsigned in_en: 1, in_invert: 1, out_en: 1, out_invert: 1; } flags;
} dedic_gpio_bundle_config_t;
static inline esp_err_t dedic_gpio_new_bundle(const dedic_gpio_bundle_config_t *c,
                                              dedic_gpio_bundle_handle_t *h) {
    (void)c; *h = NULL; return ESP_OK;
}
static inline esp_err_t dedic_gpio_del_bundle(dedic_gpio_bundle_handle_t h) { (void)h; return ESP_OK; }
static inline uint32_t dedic_gpio_bundle_read_in(dedic_gpio_bundle_handle_t h) { (void)h; return 0; }
static inline void dedic_gpio_bundle_write(dedic_gpio_bundle_handle_t h, uint32_t mask, uint32_t val) { (void)h;(void)mask;(void)val; }
static inline esp_err_t dedic_gpio_get_out_offset(dedic_gpio_bundle_handle_t h, uint32_t *o) { (void)h; *o=0; return ESP_OK; }
static inline esp_err_t dedic_gpio_get_in_offset(dedic_gpio_bundle_handle_t h, uint32_t *o) { (void)h; *o=0; return ESP_OK; }
