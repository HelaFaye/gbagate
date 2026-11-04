/*
 * test_gbprinter.c -- src/gbprinter.c, the Game Boy Printer decoder, from
 * packets built the way the protocol frames them.
 */
#include <stdio.h>
#include <string.h>

#include "gbp_png.h"
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

static void feed_pkt(uint8_t cmd, uint8_t comp, const uint8_t *d, uint16_t n) {
    gbp_feed(&g_p, g_buf, pkt(g_buf, cmd, comp, d, n, 0));
}

/* A band of one colour. */
static void solid(uint8_t band[GBP_BAND_BYTES], int colour) {
    for (int i = 0; i < GBP_BAND_BYTES; i += 2) {
        band[i] = colour & 1 ? 0xFF : 0;
        band[i + 1] = colour & 2 ? 0xFF : 0;
    }
}

/* A game prints a long picture in parts; the paper feeds out at the end. */
static void test_parts(void) {
    uint8_t band[GBP_BAND_BYTES];
    gbp_init(&g_p);
    const uint8_t part[4] = { 1, 0x10, 0xE4, 0x40 };    /* a margin before, none after */
    const uint8_t last[4] = { 1, 0x03, 0x1B, 0x40 };    /* inverted; three lines after */
    feed_pkt(0x01, 0, NULL, 0);
    solid(band, 1);
    feed_pkt(0x04, 0, band, GBP_BAND_BYTES);
    feed_pkt(0x04, 0, NULL, 0);
    feed_pkt(0x02, 0, part, 4);
    check("parts: the first PRINT has no margin after: not finished", g_p.finished, 0);
    check("... its rows are printed", g_p.committed, 16);
    feed_pkt(0x01, 0, NULL, 0);
    check("parts: INIT between parts keeps the printed rows", g_p.bands, 1);
    solid(band, 1);
    feed_pkt(0x04, 0, band, GBP_BAND_BYTES);
    feed_pkt(0x04, 0, band, GBP_BAND_BYTES);
    feed_pkt(0x04, 0, NULL, 0);
    feed_pkt(0x02, 0, last, 4);
    check("parts: the margin after finishes the picture", g_p.finished, 1);
    check("... one picture", g_p.pictures, 1);
    check("... of all three bands", gbp_rows(&g_p), 48);
    check("each part through its own palette: E4 keeps colour 1", gbp_shade(&g_p, 0, 0), 1);
    check("... 1B makes colour 1 shade 2", gbp_shade(&g_p, 20, 0), 2);
    feed_pkt(0x0F, 0, NULL, 0);
    feed_pkt(0x02, 0, last, 4);
    check("a PRINT with nothing new: no second picture", g_p.pictures, 1);
    check("... and the picture stays", gbp_rows(&g_p), 48);
    feed_pkt(0x04, 0, band, GBP_BAND_BYTES);
    check("the next band starts a new picture", gbp_rows(&g_p), 16);
    check("... nothing printed of it yet", g_p.committed, 0);
    feed_pkt(0x01, 0, NULL, 0);
    check("INIT drops bands never printed", g_p.bands, 0);

    feed_pkt(0x04, 0, band, GBP_BAND_BYTES);
    feed_pkt(0x02, 0, part, 4);
    feed_pkt(0x04, 0, band, GBP_BAND_BYTES);           /* sent, never printed */
    check("finish: the game stopped without a margin", gbp_finish(&g_p), 1);
    check("... a picture of what was printed", gbp_rows(&g_p) == 16 && g_p.pictures == 2, 1);
    check("... once", gbp_finish(&g_p), 0);
    gbp_init(&g_p);
    check("finish with nothing printed: no picture", gbp_finish(&g_p) || g_p.pictures, 0);
}

/* What GB-Link's printer mode sends its host (gbprinter.h). */
static size_t spkt(uint8_t *o, uint8_t cmd, uint8_t comp, const uint8_t *d, uint16_t n) {
    size_t k = 0;
    o[k++] = cmd; o[k++] = comp; o[k++] = (uint8_t)n; o[k++] = (uint8_t)(n >> 8);
    memcpy(o + k, d, n);
    return k + n;
}

static void test_stream(void) {
    static uint8_t s[8192];
    uint8_t band[GBP_BAND_BYTES];
    const uint8_t pr[4] = { 1, 0x13, 0xE4, 0x40 };
    size_t k = 0;
    s[k++] = 0xFF;                                     /* the mode started */
    k += spkt(s + k, 0x01, 0, NULL, 0);
    k += spkt(s + k, 0x0F, 0, NULL, 0);
    for (int i = 0; i < 9; i++) { solid(band, i & 3); k += spkt(s + k, 0x04, 0, band, GBP_BAND_BYTES); }
    k += spkt(s + k, 0x04, 0, NULL, 0);
    k += spkt(s + k, 0x02, 0, pr, 4);
    s[k++] = 0xFE;                                     /* the PRINT answered */
    gbp_init(&g_p);
    for (size_t i = 0; i < k; i += 64) gbp_feed_stream(&g_p, s + i, k - i < 64 ? k - i : 64);
    check("stream: every packet, through 64-byte sends", g_p.packets, 13);
    check("... a 160 x 144 picture", gbp_rows(&g_p), 144);
    check("... finished", g_p.pictures, 1);
    check("... band 3 is colour 3", gbp_shade(&g_p, 3 * 16 + 5, 100), 3);
    check("... nothing counted as damaged", g_p.bad_checksums, 0);

    const char ab[] = "ABORTPRINT";
    gbp_feed_stream(&g_p, (const uint8_t *)ab, 10);
    check("stream: ABORTPRINT is counted", g_p.aborts, 1);
    check("... and is not a packet", g_p.packets, 13);
    k = spkt(s, 0x01, 0, NULL, 0);
    gbp_feed_stream(&g_p, s, k);
    check("... the next packet is read", g_p.packets, 14);
    const uint8_t junk[4] = { 0x04, 0x00, 0xFF, 0xFF };   /* a length no packet has */
    gbp_feed_stream(&g_p, junk, 4);
    check("stream: an impossible length is refused", g_p.bad_checksums, 1);
}

/* The saved picture: a PNG anything opens. Checked from its bytes. */
static uint32_t rd32(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

static uint32_t crc32_ref(const uint8_t *b, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

static void test_png(void) {
    static uint8_t png[GBP_PNG_MAX];
    uint8_t band[GBP_BAND_BYTES];
    const uint8_t pr[4] = { 1, 0x01, 0xE4, 0x40 };
    gbp_init(&g_p);
    check("png: no picture, no file", gbp_png(&g_p, png, sizeof png), 0);
    for (int i = 0; i < 4; i++) { solid(band, i); feed_pkt(0x04, 0, band, GBP_BAND_BYTES); }
    g_p.pixels[0][1] = 3;                              /* one dark pixel on white */
    feed_pkt(0x02, 0, pr, 4);
    const size_t n = gbp_png(&g_p, png, sizeof png);
    check("png: written", n > 0, 1);
    check("png: its size is GBP_PNG_SIZE", (long)n, (long)GBP_PNG_SIZE(64));
    check("png: too small a buffer is refused", gbp_png(&g_p, png, n - 1), 0);
    check("png: the signature", memcmp(png, "\x89PNG\r\n\x1a\n", 8), 0);

    /* walk the chunks, checking each CRC */
    size_t at = 8, idat = 0, idat_len = 0;
    int chunks = 0, crc_ok = 1;
    while (at + 12 <= n) {
        const uint32_t len = rd32(png + at);
        if (crc32_ref(png + at + 4, len + 4) != rd32(png + at + 8 + len)) crc_ok = 0;
        if (!memcmp(png + at + 4, "IHDR", 4)) {
            check("png: 160 wide", rd32(png + at + 8), 160);
            check("png: 64 high", rd32(png + at + 12), 64);
            check("png: 2-bit greyscale", png[at + 16] == 2 && png[at + 17] == 0, 1);
        }
        if (!memcmp(png + at + 4, "IDAT", 4)) { idat = at + 8; idat_len = len; }
        at += 12 + len;
        chunks++;
    }
    check("png: IHDR, IDAT, IEND, to the last byte", chunks == 3 && at == n, 1);
    check("png: every CRC right", crc_ok, 1);

    /* the zlib stream: header, one stored block, the rows, Adler-32 */
    const uint8_t *z = png + idat;
    const size_t raw = 64 * GBP_PNG_ROW_BYTES;
    check("png: zlib header", z[0] == 0x78 && (z[0] * 256 + z[1]) % 31 == 0, 1);
    check("png: one final stored block of the rows",
          z[2] == 1 && (z[3] | z[4] << 8) == (long)raw && (uint16_t)(z[5] | z[6] << 8) == (uint16_t)~raw, 1);
    const uint8_t *rows = z + 7;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; i++) { a = (a + rows[i]) % 65521; b = (b + a) % 65521; }
    check("png: Adler-32", rd32(rows + raw), b << 16 | a);
    check("png: ... and nothing after it", idat_len, 2 + 5 + raw + 4);
    check("png: every row unfiltered", rows[0] == 0 && rows[GBP_PNG_ROW_BYTES] == 0, 1);
    check("png: white paper is grey 3, the dark pixel 0", rows[1], 0xCF);   /* 11 00 11 11 */
    check("png: band 3 (colour 3) is black", rows[1 + 3 * 16 * GBP_PNG_ROW_BYTES + 5], 0);
    check("png: band 1 is grey 2", rows[1 + 16 * GBP_PNG_ROW_BYTES], 0xAA);
    check("png: a full-length print fits GBP_PNG_MAX", GBP_PNG_MAX < 12 * 1024, 1);

    char name[GBP_NAME_MAX];
    gbp_png_name(name, 7);
    check("name: PRNT0007.PNG", strcmp(name, "PRNT0007.PNG"), 0);
    check("name: read back", gbp_png_number(name), 7);
    check("name: any case", gbp_png_number("prnt0123.png"), 123);
    check("name: not ours", gbp_png_number("PRNT12A4.PNG") + gbp_png_number("EMERALD.GBA") +
                            gbp_png_number("PRNT0001.BMP") + gbp_png_number(NULL), 0);
}

int main(void) {
    test_parts();
    test_stream();
    test_png();
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
