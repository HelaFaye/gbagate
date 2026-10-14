#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "tinyusb.h"
typedef enum { TINYUSB_USBDEV_0 } tinyusb_usbdev_t;
typedef enum { TINYUSB_CDC_ACM_0 } tinyusb_cdcacm_itf_t;
typedef enum { CDC_EVENT_RX, CDC_EVENT_LINE_STATE_CHANGED } cdcacm_event_type_t;
typedef struct {
    cdcacm_event_type_t type;
    union { struct { bool dtr, rts; } line_state_changed_data; };
} cdcacm_event_t;
typedef void (*tusb_cdcacm_callback_t)(int itf, cdcacm_event_t *event);
typedef struct {
    tinyusb_usbdev_t usb_dev;
    tinyusb_cdcacm_itf_t cdc_port;
    size_t rx_unread_buf_sz;
    tusb_cdcacm_callback_t callback_rx;
    tusb_cdcacm_callback_t callback_line_state_changed;
} tinyusb_config_cdcacm_t;
static inline esp_err_t tusb_cdc_acm_init(const tinyusb_config_cdcacm_t *c) { (void)c; return ESP_OK; }
static inline size_t tinyusb_cdcacm_write_queue(int itf, const uint8_t *d, size_t n) { (void)itf;(void)d; return n; }
static inline esp_err_t tinyusb_cdcacm_write_flush(int itf, uint32_t t) { (void)itf;(void)t; return ESP_OK; }
static inline esp_err_t tinyusb_cdcacm_read(int itf, uint8_t *d, size_t n, size_t *rx) { (void)itf;(void)d;(void)n; *rx=0; return ESP_OK; }
