/*
 * gbp_png.c -- see gbp_png.h.
 */
#include "gbp_png.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static uint32_t crc_table[256];

static void crc_init(void) {
    if (crc_table[1]) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc(const uint8_t *b, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void be32(uint8_t *o, uint32_t v) {
    o[0] = (uint8_t)(v >> 24); o[1] = (uint8_t)(v >> 16); o[2] = (uint8_t)(v >> 8); o[3] = (uint8_t)v;
}

/* A chunk's length, type and CRC around the data already at o + 8. */
static size_t chunk(uint8_t *o, const char *type, size_t len) {
    be32(o, (uint32_t)len);
    memcpy(o + 4, type, 4);
    be32(o + 8 + len, crc(o + 4, len + 4));
    return 12 + len;
}

size_t gbp_png(const gbp_t *p, uint8_t *out, size_t cap) {
    const int rows = gbp_rows(p);
    if (rows == 0 || cap < (size_t)GBP_PNG_SIZE(rows)) return 0;
    crc_init();
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    size_t k = 0;
    memcpy(out, sig, 8);
    k = 8;

    uint8_t *h = out + k + 8;                     /* IHDR */
    be32(h, GBP_WIDTH);
    be32(h + 4, (uint32_t)rows);
    h[8] = 2;                                     /* bit depth */
    h[9] = 0;                                     /* greyscale */
    h[10] = h[11] = h[12] = 0;                    /* deflate, adaptive filters, no interlace */
    k += chunk(out + k, "IHDR", 13);

    uint8_t *z = out + k + 8;                     /* IDAT: zlib, stored blocks */
    size_t zn = 0;
    z[zn++] = 0x78; z[zn++] = 0x01;
    const size_t raw = (size_t)rows * GBP_PNG_ROW_BYTES;
    uint32_t a = 1, b = 0;                        /* Adler-32 of the raw rows */
    size_t done = 0;
    int r = 0, col = 0;                           /* where in the raw rows the next byte is */
    while (done < raw) {
        const size_t len = raw - done > 65535 ? 65535 : raw - done;
        z[zn++] = done + len == raw;              /* BFINAL; BTYPE 00 */
        z[zn++] = (uint8_t)len; z[zn++] = (uint8_t)(len >> 8);
        z[zn++] = (uint8_t)~len; z[zn++] = (uint8_t)(~len >> 8);
        for (size_t i = 0; i < len; i++) {
            uint8_t v = 0;                        /* filter 0 at col 0 */
            if (col) {
                const int x = (col - 1) * 4;
                for (int j = 0; j < 4; j++) v = (uint8_t)(v << 2 | (3 - gbp_shade(p, r, x + j)));
            }
            z[zn++] = v;
            a = (a + v) % 65521; b = (b + a) % 65521;
            if (++col == GBP_PNG_ROW_BYTES) { col = 0; r++; }
        }
        done += len;
    }
    be32(z + zn, b << 16 | a);
    zn += 4;
    k += chunk(out + k, "IDAT", zn);
    k += chunk(out + k, "IEND", 0);
    return k;
}

void gbp_png_name(char out[GBP_NAME_MAX], unsigned number) {
    snprintf(out, GBP_NAME_MAX, "PRNT%04u.PNG", number % 10000);
}

unsigned gbp_png_number(const char *name) {
    if (!name || strlen(name) != 12) return 0;
    static const char pre[] = "PRNT", ext[] = ".PNG";
    unsigned n = 0;
    for (int i = 0; i < 4; i++)
        if (toupper((unsigned char)name[i]) != pre[i] || toupper((unsigned char)name[8 + i]) != ext[i]) return 0;
    for (int i = 4; i < 8; i++) {
        if (!isdigit((unsigned char)name[i])) return 0;
        n = n * 10 + (unsigned)(name[i] - '0');
    }
    return n;
}
