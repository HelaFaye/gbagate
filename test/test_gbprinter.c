/*
 * test_gbprinter.c -- src/gbprinter.c, the Game Boy Printer decoder, from
 * packets built the way the protocol frames them.
 */
#include <stdio.h>
#include <string.h>

#include "gbprinter.h"

static int g_fail = 0;
static gbp_t g_p;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-58s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* 88 33 | cmd | comp | len | data | sum, then the printer's 81 00 */
static size_t pkt(uint8_t *out, uint8_t cmd, uint8_t comp, const uint8_t *d, uint16_t n, int corrupt) {
    size_t k = 0;
    uint16_t sum = (uint16_t)(cmd + comp + (n & 0xFF) + (n >> 8));
    out[k++] = 0x88; out[k++] = 0x33; out[k++] = cmd; out[k++] = comp;
    out[k++] = (uint8_t)n; out[k++] = (uint8_t)(n >> 8);
    for (uint16_t i = 0; i < n; i++) { out[k++] = d[i]; sum = (uint16_t)(sum + d[i]); }
    sum = (uint16_t)(sum + corrupt);
    out[k++] = (uint8_t)sum; out[k++] = (uint8_t)(sum >> 8);
    out[k++] = 0x81; out[k++] = 0x00;
    return k;
}

static uint8_t g_buf[4096];

int main(void) {
    gbp_init(&g_p);
    size_t n = pkt(g_buf, 0x01, 0, NULL, 0, 0);
    gbp_feed(&g_p, g_buf, n);
    check("INIT: a packet", g_p.packets, 1);

    /* a band: tile 0 all colour 3 (both planes set), tile 21 (row 2, col 1) colour 1 */
    uint8_t band[GBP_BAND_BYTES] = {0};
    memset(band, 0xFF, 16);
    for (int y = 0; y < 8; y++) band[16 * 21 + 2 * y] = 0xFF;
    n = pkt(g_buf, 0x04, 0, band, GBP_BAND_BYTES, 0);
    gbp_feed(&g_p, g_buf, 3);                          /* in pieces */
    gbp_feed(&g_p, g_buf + 3, n - 3);
    check("DATA: one band", g_p.bands, 1);
    check("... 16 rows", gbp_rows(&g_p), 16);
    check("tile 0 is colour 3", g_p.pixels[0][0] == 3 && g_p.pixels[7][7] == 3, 1);
    check("tile 1 is colour 0", g_p.pixels[0][8], 0);
    check("tile 21 (second tile row, x 8) is colour 1", g_p.pixels[8][8] == 1 && g_p.pixels[15][15] == 1, 1);

    /* the same band, compressed: a run of 16 FF, a run of 320, literals */
    uint8_t c[64];
    size_t m = 0;
    c[m++] = 0x80 | (16 - 2); c[m++] = 0xFF;                       /* tile 0 */
    for (int left = 16 * 21 - 16; left > 0; ) {                     /* zeros to tile 21 */
        int r = left > 129 ? 129 : left; c[m++] = (uint8_t)(0x80 | (r - 2)); c[m++] = 0; left -= r;
    }
    c[m++] = 15; for (int y = 0; y < 8; y++) { c[m++] = 0xFF; c[m++] = 0; }   /* 16 literals */
    for (int left = GBP_BAND_BYTES - 16 * 22; left > 0; ) {
        int r = left > 129 ? 129 : left; c[m++] = (uint8_t)(0x80 | (r - 2)); c[m++] = 0; left -= r;
    }
    n = pkt(g_buf, 0x04, 1, c, (uint16_t)m, 0);
    gbp_feed(&g_p, g_buf, n);
    check("compressed DATA: a second band", g_p.bands, 2);
    check("... decoded the same", memcmp(g_p.pixels[16], g_p.pixels[0], sizeof g_p.pixels[0] * 16), 0);

    n = pkt(g_buf, 0x04, 0, band, GBP_BAND_BYTES, 1);
    gbp_feed(&g_p, g_buf, n);
    check("a bad checksum is refused", g_p.bands, 2);
    check("... and counted", g_p.bad_checksums, 1);
    n = pkt(g_buf, 0x04, 0, NULL, 0, 0);
    gbp_feed(&g_p, g_buf, n);
    check("empty DATA ends the bands, adds none", g_p.bands, 2);

    const uint8_t print[4] = { 1, 0x13, 0x1B, 0x40 };   /* palette 1B: inverted */
    n = pkt(g_buf, 0x02, 0, print, 4, 0);
    gbp_feed(&g_p, g_buf, n);
    check("PRINT: seen", g_p.printed, 1);
    check("... its palette", g_p.palette, 0x1B);
    check("colour 3 through palette 1B is white", gbp_shade(&g_p, 0, 0), 0);
    check("colour 0 is black", gbp_shade(&g_p, 0, 8), 3);
    check("margins kept", g_p.margins, 0x13);

    const uint8_t zero[4] = { 1, 0, 0, 0x40 };
    gbp_feed(&g_p, g_buf, pkt(g_buf, 0x01, 0, NULL, 0, 0));
    check("INIT clears the picture", g_p.bands, 0);
    gbp_feed(&g_p, g_buf, pkt(g_buf, 0x02, 0, zero, 4, 0));
    check("palette 00 means the default", g_p.palette, 0xE4);

    uint8_t junk[3] = { 0x12, 0x88, 0x88 };
    gbp_feed(&g_p, junk, 3);
    gbp_feed(&g_p, g_buf, pkt(g_buf, 0x01, 0, NULL, 0, 0));
    check("junk and a doubled 88 before a packet", g_p.packets, 8);
    for (int i = 0; i < GBP_MAX_BANDS + 2; i++) gbp_feed(&g_p, g_buf, pkt(g_buf, 0x04, 0, band, GBP_BAND_BYTES, 0));
    check("bands past the most are dropped", g_p.bands, GBP_MAX_BANDS);
    check("... and counted", g_p.dropped_bands, 2);

    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
