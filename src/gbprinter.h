/*
 * gbprinter.h -- the picture a Game Boy (or a GBA running a Game Boy game)
 * sends to a Game Boy Printer, decoded from the bytes it sends.
 *
 * The firmware's printer mode (GB-Link mode 0x03) answers the game as a
 * printer and passes its bytes on; this turns them into an image the board
 * can show. The protocol, as the Game Boy Printer's is documented (the
 * printer's own command set):
 *
 *   packet   88 33 | command | compression | length (LE16) | data | checksum (LE16)
 *            the checksum is the 16-bit sum of command..data; the printer
 *            then answers 81 (alive) and a status byte, which the decoder
 *            does not need.
 *   commands 01 INIT    start a new picture
 *            04 DATA    one band: 640 bytes, 20 x 2 tiles of 16 bytes, 2 bits
 *                       a pixel (160 x 16); compressed when the flag is set:
 *                       a control byte n, bit 7 set: the next byte n-128+2
 *                       times; clear: the next n+1 bytes as they are.
 *                       DATA with length 0 ends the bands.
 *            02 PRINT   4 bytes: sheets, margins, palette, exposure. The
 *                       palette maps the 2-bit colours 0-3 to shades, two
 *                       bits each (colour c's shade is bits 2c+1..2c).
 *            0F STATUS  a poll; nothing to draw.
 *
 * A picture is up to GBP_MAX_BANDS bands; more are dropped and counted.
 *
 * A picture, as a printer makes it: a game prints a long image in parts, a
 * PRINT each, and the paper only feeds out when a PRINT asks for a margin
 * after it (the low nibble of its margins byte). So the parts are joined
 * until then; each PRINT puts the rows it printed through its own palette
 * (rows below `committed` hold shades, the rest colour indexes), and the
 * picture is finished -- `pictures` counts up, for the caller to save it --
 * when the margin comes, or when the caller decides the game has stopped
 * (gbp_finish). The next band after that starts a new picture; until then
 * the finished one stays to be shown. INIT drops bands that were never
 * printed, as the printer clears its buffer.
 *
 * Two inputs:
 *   gbp_feed         the bytes as the game sends them (88 33 ... checksum)
 *   gbp_feed_stream  what GB-Link's printer mode (third_party/gblink-firmware,
 *                    src/module/gb.cpp) passes to its host: FF when the
 *                    mode starts, then each packet as command, compression,
 *                    length (LE16) and data -- no sync, no checksum, the
 *                    firmware has already answered the game -- FE after a
 *                    PRINT's packet, and the text ABORTPRINT when the game
 *                    stopped mid-packet (what came since the last flush is
 *                    lost in the firmware).
 */
#ifndef GBPRINTER_H
#define GBPRINTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBP_WIDTH      160
#define GBP_BAND_ROWS  16
#define GBP_BAND_BYTES 640
#define GBP_MAX_BANDS  18                      /* 288 rows: a long print */
#define GBP_MAX_DATA   (GBP_BAND_BYTES + 16)   /* a compressed band can be a little longer */

typedef enum { GBP_SEEK_88, GBP_SEEK_33, GBP_CMD, GBP_COMP, GBP_LEN0, GBP_LEN1,
               GBP_DATA, GBP_SUM0, GBP_SUM1 } gbp_state_t;

typedef struct gbp {
    gbp_state_t state;
    uint8_t     cmd, comp;
    uint16_t    len, got, sum, want;
    uint8_t     data[GBP_MAX_DATA];

    /* the picture so far, row by row: shades below `committed`, colour
     * indexes 0-3 (not yet printed) from there */
    uint8_t     pixels[GBP_MAX_BANDS * GBP_BAND_ROWS][GBP_WIDTH];
    uint8_t     bands;
    uint16_t    committed;   /* rows a PRINT has put through its palette     */
    bool        finished;    /* the paper fed out: the next band starts anew */
    bool        printed;     /* a PRINT came: palette and margins are known */
    uint8_t     palette;     /* the last PRINT's; 0xE4 (identity) until then */
    uint8_t     sheets, margins, exposure;

    /* gbp_feed_stream's place in the firmware's stream */
    uint8_t     sstate, abort_at;

    uint32_t    packets, bad_checksums, dropped_bands, prints, pictures, aborts;
} gbp_t;

void gbp_init(gbp_t *p);

/* Feed what the game sent, any amount at a time. */
void gbp_feed(gbp_t *p, const uint8_t *bytes, size_t n);

/* Feed what GB-Link's printer mode sent its host. */
void gbp_feed_stream(gbp_t *p, const uint8_t *bytes, size_t n);

/* The game has stopped: finish the picture with what was printed (bands it
 * sent but never printed are dropped). True if that finished one. */
bool gbp_finish(gbp_t *p);

/* Rows decoded so far. */
static inline int gbp_rows(const gbp_t *p) { return p->bands * GBP_BAND_ROWS; }

/* The shade of a pixel: 0 white .. 3 black. Printed rows have theirs; rows
 * not printed yet are shown through the identity palette. */
static inline uint8_t gbp_shade(const gbp_t *p, int row, int col) {
    return p->pixels[row][col];        /* the identity palette: index = shade */
}

#ifdef __cplusplus
}
#endif

#endif
