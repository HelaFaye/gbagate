/*
 * settings.c -- see settings.h
 */

#include <string.h>

#include "settings.h"
#include "ui_input.h"

/* Same CRC32 as rom_catalog.c; duplicated rather than shared because settings
 * must not depend on the ROM catalogue, which is not in every image. */
uint32_t settings_crc32(const uint8_t *data, size_t len) {
    if (!data) {
        return 0;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

void settings_defaults(settings_t *s) {
    memset(s, 0, sizeof *s);
    /*
     * The 1.44" green-tab offsets, which is what most 128x128 modules want.
     * A wrong guess here shows as a two-pixel shift rather than a blank screen,
     * so it is a safe default to ship and an obvious one to correct.
     */
    s->panel    = 0;
    s->colstart = 2;
    s->rowstart = 3;
    s->bgr      = true;

    s->game  = (uint8_t)CG_GAME_BN6_US;
    s->accel = 20;
    s->wrap  = true;
    /* Encoder by default: it is the board that exists. */
    s->input_mode = (uint8_t)INPUT_ENCODER;

    s->logo_crc = 0;   /* no donor supplied: the logo check stays off */
}

/* ---- blob format --------------------------------------------------------
 *
 *   u32 magic | u16 version | u16 length | payload | u32 crc
 *
 * Length and CRC both, because NVS can return a short read as easily as a
 * corrupt one and the two need different verdicts in a log.
 */

#define HDR 8u

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* Fixed field order, so a future version can extend the tail without moving
 * anything a previous build already understands. */
static size_t pack_payload(const settings_t *s, uint8_t *p) {
    size_t n = 0;
    p[n++] = s->panel;
    p[n++] = s->colstart;
    p[n++] = s->rowstart;
    p[n++] = s->bgr ? 1u : 0u;
    p[n++] = s->game;
    p[n++] = s->accel;
    p[n++] = s->wrap ? 1u : 0u;
    p[n++] = s->input_mode;   /* was pad; SETTINGS_VERSION covers the change */
    put32(p + n, s->logo_crc); n += 4;
    memcpy(p + n, s->favorites, SETTINGS_FAV_BYTES);
    n += SETTINGS_FAV_BYTES;
    return n;
}

size_t settings_pack(const settings_t *s, uint8_t *buf, size_t cap) {
    if (!s || !buf) {
        return 0;
    }
    uint8_t payload[SETTINGS_BLOB_MAX];
    size_t plen = pack_payload(s, payload);

    size_t total = HDR + plen + 4;
    if (total > cap) {
        return 0;
    }

    put32(buf, SETTINGS_MAGIC);
    put16(buf + 4, SETTINGS_VERSION);
    put16(buf + 6, (uint16_t)plen);
    memcpy(buf + HDR, payload, plen);
    put32(buf + HDR + plen, settings_crc32(buf, HDR + plen));
    return total;
}

bool settings_unpack(settings_t *s, const uint8_t *buf, size_t len) {
    settings_defaults(s);

    if (!buf || len < HDR + 4) {
        return false;
    }
    if (get32(buf) != SETTINGS_MAGIC) {
        return false;
    }
    if (get16(buf + 4) != SETTINGS_VERSION) {
        /*
         * A blob from another version is not corrupt, but this build cannot
         * read it. Defaults are the recoverable answer; guessing at the layout
         * would silently apply someone else's calibration.
         */
        return false;
    }

    size_t plen = get16(buf + 6);
    if (plen > SETTINGS_BLOB_MAX || len < HDR + plen + 4) {
        return false;
    }
    if (get32(buf + HDR + plen) != settings_crc32(buf, HDR + plen)) {
        return false;
    }

    const uint8_t *p = buf + HDR;
    size_t n = 0;
    s->panel    = p[n++];
    s->colstart = p[n++];
    s->rowstart = p[n++];
    s->bgr      = p[n++] != 0;
    s->game     = p[n++];
    s->accel    = p[n++];
    s->wrap       = p[n++] != 0;
    s->input_mode = p[n++];
    s->logo_crc = get32(p + n); n += 4;
    if (plen >= n + SETTINGS_FAV_BYTES) {
        memcpy(s->favorites, p + n, SETTINGS_FAV_BYTES);
    }

    /*
     * Clamp rather than trust. A blob can be intact and still hold a value this
     * build cannot use -- a game index from a build with more games, an offset
     * past the panel. Clamping keeps a valid save usable; rejecting it would
     * throw away the calibration too.
     */
    if (s->panel > 1)  s->panel = 0;
    if (s->game >= CG_GAME_COUNT) s->game = (uint8_t)CG_GAME_BN6_US;
    if (s->colstart > 8) s->colstart = 2;
    if (s->rowstart > 8) s->rowstart = 3;
    if (s->accel == 0 || s->accel > 40) s->accel = 20;
    if (s->input_mode >= INPUT_MODE_COUNT) {
        s->input_mode = (uint8_t)INPUT_ENCODER;
    }
    return true;
}

bool settings_load(settings_t *s, const settings_io_t *io, void *ctx) {
    settings_defaults(s);
    if (!io || !io->load) {
        return false;
    }
    uint8_t buf[SETTINGS_BLOB_MAX + 16];
    size_t len = 0;
    if (!io->load(ctx, buf, sizeof buf, &len)) {
        return false;   /* nothing stored yet: normal on a new device */
    }
    return settings_unpack(s, buf, len);
}

bool settings_save(const settings_t *s, const settings_io_t *io, void *ctx) {
    if (!io || !io->save) {
        return false;
    }
    uint8_t buf[SETTINGS_BLOB_MAX + 16];
    size_t len = settings_pack(s, buf, sizeof buf);
    if (len == 0) {
        return false;
    }
    return io->save(ctx, buf, len);
}
