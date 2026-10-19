/*
 * gblink_hw_esp32s3.c -- the board parts the GB-Link 0x4x commands reach,
 * the settings partition, and the kernel-shim storage hooks.
 *
 *   LED        WS2812 over RMT on PIN_RGB_LED, when the board has one.
 *   VCC        a 3.3 V / 5 V switch, when the board has one (none do yet).
 *   reboot     esp_restart().
 *   bootloader the ROM's USB download mode -- the ESP32-S3 equivalent of the
 *              RP2040 BOOTSEL that 0x43 asks for. The GB-Link launcher then
 *              waits for a Pico bootloader that never appears and times out;
 *              esptool (or the ESP Web Tools page) can flash from here.
 *   storage    upstream's 4 KB settings sector becomes one NVS blob. Only the
 *              first 64 bytes are ever written by persist.cpp (a 22-byte
 *              struct), so that is what is kept; the rest reads as erased.
 *
 * NOT YET RUN ON HARDWARE.
 */
#include <string.h>

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "soc/rtc_cntl_reg.h"

#include "board.h"
#include "gblink_port.h"
#include "zshim.h"
#include <zephyr/storage/flash_map.h>

static const char *TAG = "gblink_hw";

/* ---- LED ------------------------------------------------------------------- */

#if BOARD_HAS_RGB_LED
static rmt_channel_handle_t s_rmt;
static rmt_encoder_handle_t s_enc;

static void led_init(void) {
    if (s_rmt) return;
    rmt_tx_channel_config_t ch = {
        .gpio_num = PIN_RGB_LED,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 100 ns ticks */
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    if (rmt_new_tx_channel(&ch, &s_rmt) != ESP_OK) { s_rmt = NULL; return; }
    rmt_bytes_encoder_config_t bc = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    rmt_new_bytes_encoder(&bc, &s_enc);
    rmt_enable(s_rmt);
}
#endif

void gblink_hw_set_led(uint8_t r, uint8_t g, uint8_t b, bool on) {
#if BOARD_HAS_RGB_LED
    led_init();
    if (!s_rmt) return;
    uint8_t grb[3] = { on ? g : 0, on ? r : 0, on ? b : 0 };
    rmt_transmit_config_t tc = { .loop_count = 0 };
    rmt_transmit(s_rmt, s_enc, grb, sizeof grb, &tc);
#else
    (void)r; (void)g; (void)b; (void)on;
#endif
}

void gblink_hw_set_vcc_5v(bool five_volt) {
#if BOARD_HAS_VCC_SWITCH
#error "wire the VCC switch here"
#else
    ESP_LOGI(TAG, "VCC %s requested; this board's link VCC is fixed",
             five_volt ? "5 V" : "3.3 V");
#endif
}

void gblink_hw_reboot(void) { esp_restart(); }

void gblink_hw_reboot_to_bootloader(void) {
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

void sys_reboot(int type) { (void)type; esp_restart(); }

/* ---- storage --------------------------------------------------------------------- */

#define NVS_NS   "gblink"
#define NVS_KEY  "settings"
#define KEPT     64u

static uint8_t s_mirror[ZSHIM_STORAGE_SIZE];
static bool    s_loaded;

static void load(void) {
    if (s_loaded) return;
    memset(s_mirror, 0xFF, sizeof s_mirror);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = KEPT;
        nvs_get_blob(h, NVS_KEY, s_mirror, &n);
        nvs_close(h);
    }
    s_loaded = true;
}

int zshim_storage_read(size_t off, void *dst, size_t len) {
    load();
    memcpy(dst, s_mirror + off, len);
    return 0;
}

int zshim_storage_write(size_t off, const void *src, size_t len) {
    load();
    const uint8_t *s = src;
    for (size_t i = 0; i < len; i++) s_mirror[off + i] &= s[i]; /* NOR semantics */
    if (off >= KEPT) return 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -EIO;
    esp_err_t e = nvs_set_blob(h, NVS_KEY, s_mirror, KEPT);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -EIO;
}

int zshim_storage_erase(size_t off, size_t len) {
    load();
    memset(s_mirror + off, 0xFF, len);
    return 0;
}
