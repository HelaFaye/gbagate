#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
typedef enum { SPI1_HOST, SPI2_HOST, SPI3_HOST } spi_host_device_t;
typedef struct spi_device_t *spi_device_handle_t;
typedef struct {
    uint32_t flags; size_t length; size_t rxlength; void *user;
    const void *tx_buffer; void *rx_buffer;
    uint8_t tx_data[4]; uint8_t rx_data[4];
} spi_transaction_t;
typedef void (*transaction_cb_t)(spi_transaction_t *t);
typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;
typedef struct {
    int clock_speed_hz, mode, spics_io_num, queue_size, flags;
    transaction_cb_t pre_cb, post_cb;
} spi_device_interface_config_t;
#define SPI_DMA_CH_AUTO 3
#define SPI_DMA_DISABLED 0
#define SPI_TRANS_USE_TXDATA (1<<0)
#define SPI_TRANS_USE_RXDATA (1<<1)
#define SPI_DEVICE_HALFDUPLEX (1<<2)
#define SPI_DEVICE_NO_DUMMY   (1<<4)
#define SPI_DEVICE_3WIRE      (1<<1)
static inline esp_err_t spi_bus_initialize(spi_host_device_t h, const spi_bus_config_t *c, int d) { (void)h;(void)c;(void)d; return ESP_OK; }
static inline esp_err_t spi_bus_free(spi_host_device_t h) { (void)h; return ESP_OK; }
static inline esp_err_t spi_bus_add_device(spi_host_device_t h, const spi_device_interface_config_t *c, spi_device_handle_t *d) { (void)h;(void)c;*d=NULL; return ESP_OK; }
static inline esp_err_t spi_bus_remove_device(spi_device_handle_t d) { (void)d; return ESP_OK; }
static inline esp_err_t spi_device_transmit(spi_device_handle_t d, spi_transaction_t *t) { (void)d;(void)t; return ESP_OK; }
static inline esp_err_t spi_device_polling_transmit(spi_device_handle_t d, spi_transaction_t *t) { (void)d;(void)t; return ESP_OK; }
