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
