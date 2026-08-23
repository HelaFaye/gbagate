/*
 * app_main.c -- bring-up, in the order the hardware needs.
 *
 * Not yet run on hardware.
 */

#include "esp_log.h"
#include "nvs_flash.h"

#include "hw_caps.h"
#include "variant.h"

static const char *TAG = "main";

extern void tasks_start(uint32_t features);
extern esp_err_t st7735_init(int panel);
#ifndef GBAGATE_QEMU
extern esp_err_t msc_storage_init(void);
#endif

#ifndef VARIANT
#error "platformio.ini must define VARIANT"
#endif

void app_main(void) {
    ESP_LOGI(TAG, "gbagate %s", GBAGATE_VARIANT_NAME);

    /*
     * Resolve the image before touching hardware. A dependency error here is a
     * build mistake that reached a device, and it is far better to say so on
     * the console than to half-start and fail later in something obscure.
     */
    variant_result_t v = variant_resolve(VARIANT);
    if (v.status != VARIANT_VALID) {
        ESP_LOGE(TAG, "incoherent image: %s (%s)",
                 variant_status_name(v.status), feature_name(v.offender));
        return;
    }

    char buf[420];
    variant_describe(v.resolved, buf, sizeof buf);
    ESP_LOGI(TAG, "features: %s", buf);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /*
     * Probe before starting anything. hw_caps decides which menu entries exist,
     * and a mode whose hardware is absent must never appear -- see the
     * diagnostics screen, which is reachable with no capabilities at all.
     */
    uint32_t caps = 0;
    if (v.resolved & FEAT_DISPLAY) {
        if (st7735_init(0) == ESP_OK) {
            caps |= HW_CAP_DISPLAY;
        }
    }
#ifndef GBAGATE_QEMU
    if (v.resolved & FEAT_USB_MSC) {
        if (msc_storage_init() == ESP_OK) {
            caps |= HW_CAP_USB;
        }
    }
#endif
    if (v.resolved & FEAT_ESPNOW) {
        caps |= HW_CAP_WIFI;
    }

    tasks_start(v.resolved);
}
