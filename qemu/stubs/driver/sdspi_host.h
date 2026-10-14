#pragma once
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
typedef struct { gpio_num_t gpio_cs; spi_host_device_t host_id; } sdspi_device_config_t;
#define SDSPI_HOST_DEFAULT() ((sdmmc_host_t){ 0, 20000, 0 })
#define SDSPI_DEVICE_CONFIG_DEFAULT() ((sdspi_device_config_t){ 0, SPI2_HOST })
