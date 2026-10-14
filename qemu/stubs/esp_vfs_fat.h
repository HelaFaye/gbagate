#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
typedef struct {
    bool   format_if_mount_failed;
    int    max_files;
    size_t allocation_unit_size;
} esp_vfs_fat_mount_config_t;
#define CONFIG_WL_SECTOR_SIZE 4096

/* SD card mounting, for the mini board's storage. */
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
typedef struct {
    bool   format_if_mount_failed;
    int    max_files;
    size_t allocation_unit_size;
} esp_vfs_fat_sdmmc_mount_config_t;
static inline esp_err_t esp_vfs_fat_sdspi_mount(
    const char *base, const sdmmc_host_t *host, const sdspi_device_config_t *slot,
    const esp_vfs_fat_sdmmc_mount_config_t *cfg, sdmmc_card_t **out) {
    (void)base; (void)host; (void)slot; (void)cfg; *out = NULL; return ESP_OK;
}
static inline esp_err_t esp_vfs_fat_sdcard_unmount(const char *base, sdmmc_card_t *c) {
    (void)base; (void)c; return ESP_OK;
}
