#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"
typedef int32_t wl_handle_t;
#define WL_INVALID_HANDLE (-1)
static inline esp_err_t wl_mount(const esp_partition_t *p, wl_handle_t *h) { (void)p; *h = 0; return ESP_OK; }
static inline esp_err_t wl_unmount(wl_handle_t h) { (void)h; return ESP_OK; }
