#include "radio_mode_esp32s3.h"

#include "net_esp32s3.h"

/* The peer client is compiled only in hardware images. */
extern esp_err_t wireless_cable_start(uint8_t mode, uint8_t variant);
extern esp_err_t wireless_cable_stop_checked(void);

static radio_mode_manager_t s_manager;
static bool s_initialized;
static uint8_t s_wireless_mode = 0x01;
static uint8_t s_wireless_variant;

static int online_start(void *ctx) {
    (void)ctx;
    return (int)net_resume(false);
}

static int online_stop(void *ctx) {
    (void)ctx;
    /* net_suspend first gates reconnects, then stops the local Celio session
     * and waits for the transport worker before releasing Wi-Fi. */
    return (int)net_suspend();
}

static int wireless_start(void *ctx) {
    (void)ctx;
    return (int)wireless_cable_start(s_wireless_mode, s_wireless_variant);
}

static int wireless_stop(void *ctx) {
    (void)ctx;
    return (int)wireless_cable_stop_checked();
}

void radio_mode_esp32s3_init(void) {
    if (s_initialized) return;
    const radio_mode_ops_t ops[RADIO_MODE_COUNT] = {
        [RADIO_MODE_ONLINE] = { online_start, online_stop, NULL },
        [RADIO_MODE_WIRELESS] = { wireless_start, wireless_stop, NULL },
        /* Deliberately no SDR_RX callback: no compatible capture backend has
         * been selected or validated. capture_begin therefore fails closed. */
    };
    radio_mode_manager_init(&s_manager, ops);
    s_initialized = true;
}

radio_mode_result_t radio_mode_esp32s3_select(radio_mode_t target) {
    radio_mode_esp32s3_init();
    /* The Online owner may have been established by net_boot before the UI
     * entered its page. Also repair a prior fail-closed partial suspend when
     * Online is selected again; net_resume is safe to call repeatedly. */
    if (target == RADIO_MODE_ONLINE &&
        radio_mode_current(&s_manager) == RADIO_MODE_ONLINE) {
        return net_resume(false) == ESP_OK ? RADIO_MODE_OK : RADIO_MODE_START_FAILED;
    }
    return radio_mode_select(&s_manager, target);
}

void radio_mode_esp32s3_set_wireless(uint8_t mode, uint8_t variant) {
    s_wireless_mode = mode;
    s_wireless_variant = variant;
}

radio_mode_result_t radio_mode_esp32s3_capture_begin(void) {
    radio_mode_esp32s3_init();
    return radio_mode_capture_begin(&s_manager);
}

radio_mode_result_t radio_mode_esp32s3_capture_end(void) {
    radio_mode_esp32s3_init();
    return radio_mode_capture_end(&s_manager);
}
