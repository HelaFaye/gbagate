/*
 * settings.h -- what survives a power cycle, and how.
 *
 * Two kinds of state end up here and they are worth telling apart:
 *
 *   calibration -- the ST7735's column and row offsets, and whether the panel
 *                  is BGR or RGB. Found once with a test pattern, never
 *                  guessed again. Losing these means recalibrating by eye.
 *   preference  -- default game, encoder acceleration, favourites.
 *
 * Serialisation is here, storage is injected. That keeps the format testable on
 * a host and the NVS calls in one small file, and it means a corrupt or
 * older-version blob is handled by tested code rather than by whatever NVS
 * happens to return.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chipgate.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_MAGIC   0x47425354u   /* "GBST" */
#define SETTINGS_VERSION 1

/* One bit per chip record, matching CHIPVIEW_FAV_BYTES. */
#define SETTINGS_FAV_BYTES 64

typedef struct {
    /* calibration */
    uint8_t  panel;        /* 0 = 128x128, 1 = 128x160 */
    uint8_t  colstart;
    uint8_t  rowstart;
    bool     bgr;

    /* preference */
    uint8_t  game;         /* cg_game_t */
    uint8_t  accel;        /* encoder detents multiplier at full speed */
    bool     wrap;         /* list wraps at the ends */

    /* the logo fingerprint, 0 when the user has not supplied a donor */
    uint32_t logo_crc;

    uint8_t  favorites[SETTINGS_FAV_BYTES];
} settings_t;

/* Everything a device with no stored blob should start with. */
void settings_defaults(settings_t *s);

/*
 * Pack into a self-describing blob: magic, version, payload, CRC. Returns the
 * length written, or 0 if the buffer is too small.
 */
size_t settings_pack(const settings_t *s, uint8_t *buf, size_t cap);

/*
 * Unpack. Returns false and leaves *s at defaults if the blob is truncated,
 * corrupt, or from a version this build does not understand -- a device that
 * boots to defaults is recoverable, one that boots to garbage calibration is
 * a puzzle.
 */
bool settings_unpack(settings_t *s, const uint8_t *buf, size_t len);

#define SETTINGS_BLOB_MAX 96

/* Storage, injected. Both return false on any failure; absence is not one. */
typedef struct {
    bool (*load)(void *ctx, uint8_t *buf, size_t cap, size_t *len);
    bool (*save)(void *ctx, const uint8_t *buf, size_t len);
} settings_io_t;

/* Load, falling back to defaults. Returns true if a stored blob was used. */
bool settings_load(settings_t *s, const settings_io_t *io, void *ctx);

/* Save. Returns false if the write failed. */
bool settings_save(const settings_t *s, const settings_io_t *io, void *ctx);

uint32_t settings_crc32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_H */
