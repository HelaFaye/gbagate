/*
 * printer_mode.h -- the GB Printer mode's work, apart from the board: take
 * what the adapter's printer mode sent (port/gblink/printer_glue.cpp),
 * decode it (src/gbprinter.h), and store each finished print as a PNG
 * (src/gbp_png.h) on the board's drive.
 *
 * A picture is finished when the game feeds the paper out (a PRINT with a
 * margin after it), or, for a game that never does, once nothing has come
 * for PRINTER_IDLE_MS after a PRINT. Each finished picture is saved once.
 *
 * The board supplies the stream and the drive; the host tests supply both
 * from scripts.
 */
#ifndef PRINTER_MODE_H
#define PRINTER_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gbp_png.h"
#include "gbprinter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PRINTER_IDLE_MS 3000

typedef struct {
    /* What the adapter has sent since the last call; 0 when nothing. */
    size_t (*read)(void *ctx, uint8_t *out, size_t cap);
    /* Store a PNG under the next free print name; that name into `name`.
     * False when there is no drive to store it on, or the write failed. */
    bool (*save)(void *ctx, const uint8_t *png, size_t n, char *name, size_t cap);
    void *ctx;
} printer_io_t;

typedef struct {
    gbp_t        gbp;
    printer_io_t io;
    uint32_t     last_ms;        /* when bytes last came                  */
    uint32_t     saved;          /* pictures handled (saved or not)       */
    uint32_t     saves, save_failures;
    char         note[28];       /* for the page: "saved PRNT0001.PNG"    */
    uint8_t      png[GBP_PNG_MAX];   /* the file being saved              */
} printer_mode_t;   /* ~60 KB: the board allocates it while the mode runs */

void printer_mode_init(printer_mode_t *m, const printer_io_t *io, uint32_t now_ms);

/* Take what came, finish and save what is done. True when the page should be
 * redrawn: the picture or the note changed. */
bool printer_mode_tick(printer_mode_t *m, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
