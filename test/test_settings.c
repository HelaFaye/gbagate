/*
 * test_settings.c -- what survives a power cycle.
 *
 *   cc -std=c11 -Wall -Wextra -o test_settings test_settings.c settings.c chipgate.c
 *
 * The cases that matter are the bad ones. A device that boots to defaults after
 * a corrupt save is recoverable; one that boots to garbage calibration is a
 * puzzle, and the user has no way to tell which happened.
 */

#include <stdio.h>
#include <string.h>

#include "settings.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-46s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* ---- a simulated NVS ----------------------------------------------------- */

typedef struct {
    uint8_t blob[128];
    size_t  len;
    bool    empty;
    bool    read_fails;
    bool    write_fails;
} store_t;

static bool st_load(void *ctx, uint8_t *buf, size_t cap, size_t *len) {
    store_t *s = ctx;
    if (s->empty || s->read_fails || s->len > cap) {
        return false;
    }
    memcpy(buf, s->blob, s->len);
    *len = s->len;
    return true;
}

static bool st_save(void *ctx, const uint8_t *buf, size_t len) {
    store_t *s = ctx;
    if (s->write_fails || len > sizeof s->blob) {
        return false;
    }
    memcpy(s->blob, buf, len);
    s->len = len;
    s->empty = false;
    return true;
}

static const settings_io_t IO = { st_load, st_save };

/* ---- tests --------------------------------------------------------------- */

static void test_round_trip(void) {
    printf("round trip\n");

    settings_t a;
    settings_defaults(&a);
    a.panel = 1; a.colstart = 2; a.rowstart = 1; a.bgr = false;
    a.game = CG_GAME_BN5; a.accel = 12; a.wrap = false;
    a.logo_crc = 0xFBF64CCCu;
    a.favorites[0] = 0x81;
    a.favorites[SETTINGS_FAV_BYTES - 1] = 0x42;

    store_t store;
    memset(&store, 0, sizeof store);
    store.empty = true;

    check("saved", settings_save(&a, &IO, &store), 1);

    settings_t b;
    check("loaded", settings_load(&b, &IO, &store), 1);

    check("panel", b.panel, a.panel);
    check("colstart", b.colstart, a.colstart);
    check("rowstart", b.rowstart, a.rowstart);
    check("colour order", b.bgr, a.bgr);
    check("game", b.game, a.game);
    check("accel", b.accel, a.accel);
    check("wrap", b.wrap, a.wrap);
    check("logo fingerprint", (long)b.logo_crc, (long)a.logo_crc);
    check("favourites intact",
          memcmp(b.favorites, a.favorites, SETTINGS_FAV_BYTES), 0);
}

static void test_new_device(void) {
    printf("a device with nothing stored\n");

    store_t store;
    memset(&store, 0, sizeof store);
    store.empty = true;

    settings_t s;
    check("reports no stored blob", settings_load(&s, &IO, &store), 0);

    /* Absence is not a failure: the defaults must be usable, and in particular
     * the panel offsets must be the ones most 128x128 modules want. */
    check("panel default", s.panel, 0);
    check("colstart default", s.colstart, 2);
    check("rowstart default", s.rowstart, 3);
    check("bgr default", s.bgr, 1);
    check("accel default", s.accel, 20);
    check("logo check off by default", (long)s.logo_crc, 0);
}

static void test_corruption(void) {
    printf("corrupt and foreign blobs\n");

    settings_t a;
    settings_defaults(&a);
    a.colstart = 7;
    a.rowstart = 5;

    store_t store;
    memset(&store, 0, sizeof store);
    store.empty = true;
    settings_save(&a, &IO, &store);

    /* A flipped bit anywhere must fail the CRC, not apply half a save. */
    settings_t b;
    store.blob[10] ^= 0x01;
    check("corrupt rejected", settings_load(&b, &IO, &store), 0);
    check("fell back to defaults", b.colstart, 2);
    store.blob[10] ^= 0x01;

    /* Truncation is as likely as corruption and needs the same verdict. */
    size_t full = store.len;
    store.len = full - 3;
    check("truncated rejected", settings_load(&b, &IO, &store), 0);
    store.len = full;

    /* A blob from another version: not corrupt, but unreadable here. Guessing
     * at the layout would silently apply someone else's calibration. */
    store.blob[4] = SETTINGS_VERSION + 1;
    check("foreign version rejected", settings_load(&b, &IO, &store), 0);
    check("defaults again", b.rowstart, 3);
    store.blob[4] = SETTINGS_VERSION;

    /* Wrong magic entirely -- a different key, or uninitialised flash. */
    store.blob[0] ^= 0xFF;
    check("bad magic rejected", settings_load(&b, &IO, &store), 0);
    store.blob[0] ^= 0xFF;

    check("intact blob still loads", settings_load(&b, &IO, &store), 1);
    check("and is the one we saved", b.colstart, 7);
}

/*
 * The blob layout changed when input_mode took over the padding byte. An older
 * save has arbitrary content there, so a build that read it as a mode could
 * select input hardware that is not fitted -- and the device would then appear
 * to have dead buttons. The version bump is what prevents that.
 */
static void test_version_bump_protects_input_mode(void) {
    printf("an older save cannot set the input mode\n");

    settings_t a;
    settings_defaults(&a);
    a.input_mode = INPUT_DPAD4;

    uint8_t buf[SETTINGS_BLOB_MAX + 16];
    size_t len = settings_pack(&a, buf, sizeof buf);

    settings_t b;
    check("round trips at the current version", settings_unpack(&b, buf, len), 1);
    check("mode preserved", b.input_mode, INPUT_DPAD4);

    /* Pretend it is a version-1 blob, as a device upgraded in the field would
     * find in NVS. */
    buf[4] = 1;
    check("older blob rejected", settings_unpack(&b, buf, len), 0);
    check("and the mode is the safe default", b.input_mode, INPUT_ENCODER);

    /* An out-of-range mode in an otherwise valid blob is clamped, not
     * rejected: the calibration in the same blob is worth keeping. */
    a.input_mode = 99;
    len = settings_pack(&a, buf, sizeof buf);
    check("accepted", settings_unpack(&b, buf, len), 1);
    check("mode clamped", b.input_mode, INPUT_ENCODER);
}

static void test_clamping(void) {
    printf("valid blob, impossible values\n");

    /*
     * A save can be intact and still hold a value this build cannot use -- a
     * game index from a build with more games, an offset past the panel.
     * Clamping keeps the rest of the save; rejecting would throw the
     * calibration away with it.
     */
    settings_t a;
    settings_defaults(&a);
    a.colstart = 200;
    a.rowstart = 200;
    a.game     = 99;
    a.accel    = 0;
    a.panel    = 7;

    uint8_t buf[SETTINGS_BLOB_MAX + 16];
    size_t len = settings_pack(&a, buf, sizeof buf);
    check("packed", len > 0, 1);

    settings_t b;
    check("accepted", settings_unpack(&b, buf, len), 1);
    check("colstart clamped", b.colstart, 2);
    check("rowstart clamped", b.rowstart, 3);
    check("game clamped", b.game, CG_GAME_BN6_US);
    check("accel clamped", b.accel, 20);
    check("panel clamped", b.panel, 0);
}

static void test_io_failure(void) {
    printf("storage failure\n");

    store_t store;
    memset(&store, 0, sizeof store);

    settings_t s;
    settings_defaults(&s);

    store.write_fails = true;
    check("write failure reported", settings_save(&s, &IO, &store), 0);
    store.write_fails = false;

    store.read_fails = true;
    check("read failure reported", settings_load(&s, &IO, &store), 0);
    check("still usable", s.accel, 20);

    /* No storage at all -- an image built without persistence. */
    check("null io on load", settings_load(&s, NULL, NULL), 0);
    check("null io on save", settings_save(&s, NULL, NULL), 0);

    /* A buffer too small must refuse rather than write a partial blob. */
    uint8_t tiny[8];
    check("short buffer refused", (long)settings_pack(&s, tiny, sizeof tiny), 0);
}

/*
 * Rebuild a packed blob as an older build wrote it: the last `fields` tail
 * bytes gone, the payload length cut to match and the CRC recomputed.
 * Returns the blob's new length.
 */
static size_t drop_tail(uint8_t *buf, size_t fields) {
    size_t plen = (size_t)(buf[6] | (buf[7] << 8)) - fields;
    buf[6] = (uint8_t)plen; buf[7] = (uint8_t)(plen >> 8);
    uint32_t crc = settings_crc32(buf, 8 + plen);
    buf[8 + plen] = (uint8_t)crc; buf[9 + plen] = (uint8_t)(crc >> 8);
    buf[10 + plen] = (uint8_t)(crc >> 16); buf[11 + plen] = (uint8_t)(crc >> 24);
    return 12 + plen;
}

/* theme is the last tail field, with the same rules. */
static void test_theme_tail(void) {
    printf("theme\n");
    settings_t a, b;
    settings_defaults(&a);
    check("default is Classic", a.theme, UI_THEME_CLASSIC);
    a.theme = UI_THEME_DMG;
    a.air_link = AIR_ADAPTER;      /* not the default: "kept" must mean kept */
    uint8_t buf[SETTINGS_BLOB_MAX + 16];
    size_t n = settings_pack(&a, buf, sizeof buf);
    check("round trip", settings_unpack(&b, buf, n) && b.theme == UI_THEME_DMG, 1);
    check("a pre-theme blob still loads", settings_unpack(&b, buf, drop_tail(buf, 1)), 1);
    check("and reads as Classic", b.theme, UI_THEME_CLASSIC);
    check("keeping what it had", b.air_link, AIR_ADAPTER);
    a.theme = 200;
    n = settings_pack(&a, buf, sizeof buf);
    check("out of range clamps", settings_unpack(&b, buf, n) && b.theme == UI_THEME_CLASSIC, 1);
}

/* The USB personality rides on the blob's tail: a blob packed before the field
 * existed must still load (as GB-Link), and the field must survive a round
 * trip and be clamped when out of range. */
static void test_usb_mode_tail(void) {
    printf("usb mode\n");
    settings_t a, b;
    settings_defaults(&a);
    check("default is GB-Link", a.usb_mode, USB_MODE_GBLINK);
    a.usb_mode = USB_MODE_DRIVE;
    uint8_t buf[SETTINGS_BLOB_MAX + 16];
    size_t n = settings_pack(&a, buf, sizeof buf);
    check("round trip", settings_unpack(&b, buf, n) && b.usb_mode == USB_MODE_DRIVE, 1);

    /* As a pre-usb_mode build wrote it: without usb_mode, air_link, theme. */
    check("old blob still loads", settings_unpack(&b, buf, drop_tail(buf, 3)), 1);
    check("old blob reads as GB-Link", b.usb_mode, USB_MODE_GBLINK);

    a.usb_mode = 9;
    n = settings_pack(&a, buf, sizeof buf);
    check("out of range clamps", settings_unpack(&b, buf, n) && b.usb_mode == USB_MODE_GBLINK, 1);
}

/* air_link follows usb_mode on the tail, with the same rules. */
static void test_air_link_tail(void) {
    printf("air link\n");
    settings_t a, b;
    settings_defaults(&a);
    check("default is the Gen 3 cable", a.air_link, AIR_GEN3);
    a.air_link = AIR_ADAPTER;
    a.usb_mode = USB_MODE_DRIVE;   /* not the default: "kept" must mean kept */
    uint8_t buf[SETTINGS_BLOB_MAX + 16];
    size_t n = settings_pack(&a, buf, sizeof buf);
    check("round trip", settings_unpack(&b, buf, n) && b.air_link == AIR_ADAPTER, 1);

    /* A pre-air_link blob: without air_link and theme. */
    check("old blob still loads", settings_unpack(&b, buf, drop_tail(buf, 2)), 1);
    check("old blob reads as the cable", b.air_link, AIR_GEN3);
    check("and keeps its usb mode", b.usb_mode, USB_MODE_DRIVE);

    a.air_link = 7;
    n = settings_pack(&a, buf, sizeof buf);
    check("out of range clamps", settings_unpack(&b, buf, n) && b.air_link == AIR_GEN3, 1);

    uint8_t mode, variant;
    air_link_mode(AIR_ADAPTER, &mode, &variant);
    check("adapter is mode 0x07", mode, 0x07);
    air_link_mode(AIR_AW2, &mode, &variant);
    check("AW2 is mode 0x04 variant 2", mode == 0x04 && variant == 2, 1);
}

int main(void) {
    test_air_link_tail();
    test_theme_tail();
    test_usb_mode_tail();
    test_round_trip();
    test_new_device();
    test_corruption();
    test_version_bump_protects_input_mode();
    test_clamping();
    test_io_failure();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
