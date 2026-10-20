/*
 * sim_port.c -- gblink_port.h and the storage half of zshim.h for a host.
 *
 * The simulator speaks to its driver (a test script) over stdin/stdout in
 * records:
 *
 *     | kind:1 | ep:1 | len:2 LE | payload |
 *
 *   kind 'U'  a vendor-endpoint transfer. To the device: ep 0x01 or 0x02.
 *             From the device: ep 0x81 (status) or 0x82 (data).
 *   kind 'C'  CDC-ACM bytes, either way; ep is 0.
 *   kind 'B'  bridge UART bytes (a companion MCU, as GB-Link-Switch-LDN),
 *             either way; ep is 0.
 *   kind 'E'  an event from the simulator (ep = event code), see sim.h.
 *   kind 'G'  a GBA-model control record to the simulator, see sim_gba.c.
 *
 * So a test can stand in for WebUSB, WebSerial or both at once, and every
 * byte the vendored firmware sends is visible exactly as a host would see it.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gblink_port.h"
#include "sim.h"
#include "sim_rfu.h"
#include "zshim.h"
#include <zephyr/storage/flash_map.h>

static pthread_mutex_t s_out = PTHREAD_MUTEX_INITIALIZER;

void sim_emit(char kind, uint8_t ep, const uint8_t *data, size_t len) {
    uint8_t hdr[4] = { (uint8_t)kind, ep, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    pthread_mutex_lock(&s_out);
    fwrite(hdr, 1, 4, stdout);
    if (len) fwrite(data, 1, len, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&s_out);
}

void sim_event(uint8_t code, const void *data, size_t len) {
    sim_emit('E', code, (const uint8_t *)data, len);
}

/* ---- USB ---------------------------------------------------------------- */

bool gblink_usb_write(uint8_t ep_in, const uint8_t *data, size_t len, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (len > GBLINK_EP_SIZE) return false;
    sim_emit('U', ep_in, data, len);
    return true;
}

bool gblink_cdc_write(const uint8_t *data, size_t len) {
    sim_emit('C', 0, data, len);
    return true;
}

/* The bridge UART to a companion MCU (GB-Link-Switch-LDN): raw bytes, 'B'. */
bool gblink_bridge_write(const uint8_t *data, size_t len) {
    sim_emit('B', 0, data, len);
    return true;
}

/* ---- hardware ------------------------------------------------------------- */

static uint8_t s_led[4];
static bool    s_5v;

void gblink_hw_set_vcc_5v(bool five) {
    s_5v = five;
    uint8_t v = five;
    sim_event(SIM_EV_VCC, &v, 1);
}

void gblink_hw_set_led(uint8_t r, uint8_t g, uint8_t b, bool on) {
    s_led[0] = r; s_led[1] = g; s_led[2] = b; s_led[3] = on;
    sim_event(SIM_EV_LED, s_led, 4);
}

void gblink_hw_reboot(void)               { sim_event(SIM_EV_REBOOT, NULL, 0); }
void gblink_hw_reboot_to_bootloader(void) { sim_event(SIM_EV_BOOTLOADER, NULL, 0); }

void sys_reboot(int type) { (void)type; gblink_hw_reboot(); }

/* ---- storage: RAM, optionally loaded from and saved to a file so a test
 * can check persistence across "power cycles". ------------------------------ */

static uint8_t     s_flash[ZSHIM_STORAGE_SIZE];
static const char *s_flash_path;

void sim_storage_init(const char *path) {
    memset(s_flash, 0xFF, sizeof s_flash);
    s_flash_path = path;
    if (!path) return;
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(s_flash, 1, sizeof s_flash, f);
        (void)n;
        fclose(f);
    }
}

static void storage_save(void) {
    if (!s_flash_path) return;
    FILE *f = fopen(s_flash_path, "wb");
    if (f) {
        fwrite(s_flash, 1, sizeof s_flash, f);
        fclose(f);
    }
}

int zshim_storage_read(size_t off, void *dst, size_t len) {
    memcpy(dst, s_flash + off, len);
    return 0;
}

int zshim_storage_write(size_t off, const void *src, size_t len) {
    /* NOR semantics: a write can only clear bits. persist.cpp always erases
     * first; enforcing it here catches a vendored change that stops doing so. */
    const uint8_t *s = src;
    for (size_t i = 0; i < len; i++) s_flash[off + i] &= s[i];
    storage_save();
    sim_event(SIM_EV_FLASH_WRITE, NULL, 0);
    return 0;
}

int zshim_storage_erase(size_t off, size_t len) {
    memset(s_flash + off, 0xFF, len);
    return 0;
}

/* ---- gbagate extension identity -------------------------------------------- */

#include "gbx_proto.h"

uint32_t gbagate_ext_caps(void)  { return GBX_CAP_SD_ALT | GBX_CAP_RGB_LED; }
uint8_t  gbagate_ext_board(void) { return GBX_BOARD_HOST; }
uint8_t  gbagate_ext_image(void) { return 1; /* gate */ }

void gbagate_ext_phy_stats(uint32_t out[5]) {
    out[0] = sim_gba_words();
    out[1] = 0;                     /* the model's queue never fills */
    out[2] = 0;
    out[3] = sim_rfu_counter(SIM_RFU_GBA_WORDS);
    out[4] = 0;
}
