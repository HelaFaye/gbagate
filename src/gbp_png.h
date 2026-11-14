/*
 * gbp_png.h -- a Game Boy Printer picture (src/gbprinter.h) as a PNG file,
 * and the names prints are saved under.
 *
 * The board stores each print the emulated printer makes as an image on its
 * drive. PNG, because every computer and phone opens it; 2-bit greyscale,
 * because that is exactly what the printer has (four shades, so nothing is
 * lost or rounded), and the deflate stream is written as stored blocks: no
 * compressor to carry, and a 160 x 288 print is still under 12 KB.
 *
 * Shade 0 (paper) is white, 3 (ink) black: grey level 3 - shade.
 */
#ifndef GBP_PNG_H
#define GBP_PNG_H

#include <stddef.h>
#include <stdint.h>

#include "gbprinter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes a PNG of `rows` rows takes. */
#define GBP_PNG_ROW_BYTES (1 + GBP_WIDTH / 4)          /* filter byte + 2 bits a pixel */
#define GBP_PNG_SIZE(rows) (8 + 25 + 12 + 2 + 5 * (((rows) * GBP_PNG_ROW_BYTES + 65534) / 65535) + \
                            (rows) * GBP_PNG_ROW_BYTES + 4 + 12)
#define GBP_PNG_MAX GBP_PNG_SIZE(GBP_MAX_BANDS * GBP_BAND_ROWS)

/* The picture's rows as a PNG, into out. Returns its length, or 0 if there
 * is no picture or it does not fit `cap`. */
size_t gbp_png(const gbp_t *p, uint8_t *out, size_t cap);

/* Saved prints are PRNT0001.PNG, PRNT0002.PNG, ...: an 8.3 name, which any
 * FAT drive takes. */
#define GBP_NAME_MAX 13
void gbp_png_name(char out[GBP_NAME_MAX], unsigned number);
/* The number in a saved print's name, or 0 if the name is not one of ours
 * (any case). */
unsigned gbp_png_number(const char *name);

#ifdef __cplusplus
}
#endif

#endif
