/*
 * test_printer_mode.c -- src/printer_mode.c: the GB Printer mode from the
 * adapter's stream to a saved PNG, with a scripted stream and drive.
 */
#include <stdio.h>
#include <string.h>

#include "gbp_png.h"
#include "printer_mode.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-58s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* the adapter: a script handed out in pieces */
static uint8_t g_stream[16384];
static size_t g_len, g_pos, g_piece = 100;
static size_t rd(void *ctx, uint8_t *out, size_t cap) {
    (void)ctx;
    size_t n = g_len - g_pos;
    if (n > cap) n = cap;
    if (n > g_piece) n = g_piece;
    memcpy(out, g_stream + g_pos, n);
    g_pos += n;
    return n;
}

/* the drive */
static bool g_have_drive = true;
static unsigned g_next = 1;
static size_t g_saved_len[8];
static uint8_t g_saved_first_row[8];
static int g_saved;
static bool sv(void *ctx, const uint8_t *png, size_t n, char *name, size_t cap) {
    (void)ctx; (void)cap;
    if (!g_have_drive) return false;
    gbp_png_name(name, g_next++);
    g_saved_len[g_saved] = n;
    g_saved_first_row[g_saved] = png[8 + 25 + 8 + 2 + 5 + 1];   /* row 0's first 4 pixels */
    g_saved++;
    return true;
}

static void put(const uint8_t *b, size_t n) { memcpy(g_stream + g_len, b, n); g_len += n; }
static void spkt(uint8_t cmd, const uint8_t *d, uint16_t n) {
    const uint8_t h[4] = { cmd, 0, (uint8_t)n, (uint8_t)(n >> 8) };
    put(h, 4);
    if (n) put(d, n);
}
static void picture(int bands, uint8_t colour, uint8_t margins) {
    uint8_t band[GBP_BAND_BYTES];
    for (int i = 0; i < GBP_BAND_BYTES; i += 2) { band[i] = colour & 1 ? 0xFF : 0; band[i + 1] = colour & 2 ? 0xFF : 0; }
    spkt(0x01, NULL, 0);
    for (int i = 0; i < bands; i++) spkt(0x04, band, GBP_BAND_BYTES);
    spkt(0x04, NULL, 0);
    const uint8_t pr[4] = { 1, margins, 0xE4, 0x40 };
    spkt(0x02, pr, 4);
    const uint8_t fe = 0xFE;
    put(&fe, 1);
}

int main(void) {
    static printer_mode_t m;
    const printer_io_t io = { rd, sv, NULL };
    printer_mode_init(&m, &io, 0);
    check("nothing yet: nothing to redraw", printer_mode_tick(&m, 10), 0);

    const uint8_t ff = 0xFF;
    put(&ff, 1);
    picture(9, 3, 0x13);                    /* black, the paper fed out */
    picture(2, 1, 0x03);                    /* a second, in the same read */
    check("two prints in one go: redraw", printer_mode_tick(&m, 100), 1);
    check("... both saved", g_saved, 2);
    check("... the first is 144 rows (its own size)", (long)g_saved_len[0], (long)GBP_PNG_SIZE(144));
    check("... black: grey 0", g_saved_first_row[0], 0x00);
    check("... the second 32 rows", (long)g_saved_len[1], (long)GBP_PNG_SIZE(32));
    check("... light grey: 2", g_saved_first_row[1], 0xAA);
    check("the page says where", strcmp(m.note, "saved PRNT0002.PNG"), 0);
    check("the second is on show", gbp_rows(&m.gbp), 32);
    check("quiet: nothing to redraw", printer_mode_tick(&m, 200), 0);

    picture(3, 2, 0x10);                    /* no margin after */
    printer_mode_tick(&m, 1000);
    check("no margin after: not saved yet", g_saved, 2);
    check("... and not before the game has been quiet a while", printer_mode_tick(&m, 1000 + PRINTER_IDLE_MS - 1), 0);
    check("quiet long enough: finished", printer_mode_tick(&m, 1000 + PRINTER_IDLE_MS), 1);
    check("... and saved", g_saved, 3);
    check("... once", printer_mode_tick(&m, 1000 + 2 * PRINTER_IDLE_MS) || g_saved != 3, 0);

    g_have_drive = false;
    picture(1, 3, 0x01);
    printer_mode_tick(&m, 20000);
    check("no drive: the page says so", strcmp(m.note, "not saved: no drive"), 0);
    check("... counted", m.save_failures, 1);
    check("... the picture still shows", gbp_rows(&m.gbp), 16);

    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
