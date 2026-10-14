#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "driver/spi_master.h"
typedef struct { int slot; int max_freq_khz; uint32_t flags; } sdmmc_host_t;
typedef struct { char name[8]; } sdmmc_cid_t;
typedef struct { uint32_t capacity; uint32_t sector_size; } sdmmc_csd_t;
typedef struct { sdmmc_cid_t cid; sdmmc_csd_t csd; } sdmmc_card_t;
