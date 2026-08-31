#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef enum { ESP_PARTITION_TYPE_APP, ESP_PARTITION_TYPE_DATA } esp_partition_type_t;
typedef enum { ESP_PARTITION_SUBTYPE_DATA_FAT = 0x81, ESP_PARTITION_SUBTYPE_ANY = 0xff } esp_partition_subtype_t;
typedef struct { uint32_t address; uint32_t size; char label[17]; } esp_partition_t;
static inline const esp_partition_t *esp_partition_find_first(
    esp_partition_type_t t, esp_partition_subtype_t st, const char *label) {
    (void)t; (void)st; (void)label; return NULL;
}
