#pragma once
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
typedef enum { TINYUSB_MSC_EVENT_MOUNT_CHANGED, TINYUSB_MSC_EVENT_PREMOUNT_CHANGED } tinyusb_msc_event_type_t;
typedef struct {
    tinyusb_msc_event_type_t type;
    union { struct { bool is_mounted; } mount_changed_data; };
} tinyusb_msc_event_t;
typedef void (*tusb_msc_callback_t)(tinyusb_msc_event_t *event);
typedef struct {
    wl_handle_t wl_handle;
    tusb_msc_callback_t callback_mount_changed;
    tusb_msc_callback_t callback_premount_changed;
    esp_vfs_fat_mount_config_t mount_config;
} tinyusb_msc_spiflash_config_t;
static inline esp_err_t tinyusb_msc_storage_init_spiflash(const tinyusb_msc_spiflash_config_t *c) { (void)c; return ESP_OK; }
static inline esp_err_t tinyusb_msc_storage_mount(const char *base) { (void)base; return ESP_OK; }
static inline esp_err_t tinyusb_msc_storage_unmount(void) { return ESP_OK; }
static inline bool tinyusb_msc_storage_in_use_by_usb_host(void) { return false; }
