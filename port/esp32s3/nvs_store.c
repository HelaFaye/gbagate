/*
 * nvs_store.c -- settings.h's storage, over NVS.
 *
 * One blob under one key, rather than a namespace of typed entries. The blob
 * is versioned and CRC'd by settings.c, so a partial or stale write is caught
 * by tested code instead of arriving as a plausible value; splitting it into
 * fields would move that check into NVS, which has no opinion about whether a
 * row offset makes sense.
 *
 * Not yet run on hardware.
 */

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "settings.h"

static const char *TAG = "nvs";

#define NS  "gbagate"
#define KEY "settings"

static bool nvs_load_blob(void *ctx, uint8_t *buf, size_t cap, size_t *len) {
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;   /* namespace absent: a new device, not a fault */
    }

    size_t n = cap;
    esp_err_t err = nvs_get_blob(h, KEY, buf, &n);
    nvs_close(h);

    if (err != ESP_OK) {
        return false;
    }
    *len = n;
    return true;
}

static bool nvs_save_blob(void *ctx, const uint8_t *buf, size_t len) {
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "cannot open the settings namespace");
        return false;
    }

    esp_err_t err = nvs_set_blob(h, KEY, buf, len);
    if (err == ESP_OK) {
        /*
         * Commit explicitly. Without it the write sits in RAM and is lost on a
         * power cut -- which is exactly when someone finds out their
         * calibration was never saved.
         *
         * This is also the flash write the link task must not be running
         * across: see the deferred-save note in gate_app.c.
         */
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings write failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static const settings_io_t NVS_IO = { nvs_load_blob, nvs_save_blob };

const settings_io_t *nvs_settings_io(void) { return &NVS_IO; }
