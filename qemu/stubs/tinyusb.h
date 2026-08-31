#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef struct {
    void *device_descriptor;
    const char **string_descriptor;
    bool external_phy;
} tinyusb_config_t;
static inline esp_err_t tinyusb_driver_install(const tinyusb_config_t *c) { (void)c; return ESP_OK; }
