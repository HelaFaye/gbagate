/*
 * gbprinter.c -- see gbprinter.h.
 */
#include "gbprinter.h"

#include <string.h>

void gbp_init(gbp_t *p) {
    memset(p, 0, sizeof *p);
    p->palette = 0xE4;                         /* colour c -> shade c */
}

/* One band of 20 x 2 tiles, 16 bytes a tile (two bitplanes a row). */
static void put_band(gbp_t *p, const uint8_t *band) {
    if (p->bands >= GBP_MAX_BANDS) {
        p->dropped_bands++;
        return;
    }
    const int top = p->bands * GBP_BAND_ROWS;
    for (int t = 0; t < 40; t++) {
        const uint8_t *tile = band + 16 * t;
        const int tx = (t % 20) * 8, ty = (t / 20) * 8;
        for (int y = 0; y < 8; y++) {
            const uint8_t lo = tile[2 * y], hi = tile[2 * y + 1];
            for (int x = 0; x < 8; x++) {
                const int bit = 7 - x;
                p->pixels[top + ty + y][tx + x] = (uint8_t)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
            }
        }
    }
    p->bands++;
}

/* The band's 640 bytes, from the packet's data. False if it is malformed. */
static bool band_bytes(const gbp_t *p, uint8_t out[GBP_BAND_BYTES]) {
    if (!p->comp) {
        if (p->len != GBP_BAND_BYTES) return false;
        memcpy(out, p->data, GBP_BAND_BYTES);
        return true;
    }
    size_t o = 0, i = 0;
    while (i < p->len && o < GBP_BAND_BYTES) {
        const uint8_t c = p->data[i++];
        if (c & 0x80) {                        /* a run */
            if (i >= p->len) return false;
            const uint8_t v = p->data[i++];
            for (int k = 0; k < (c & 0x7F) + 2 && o < GBP_BAND_BYTES; k++) out[o++] = v;
        } else {                               /* literals */
            for (int k = 0; k < c + 1 && i < p->len && o < GBP_BAND_BYTES; k++) out[o++] = p->data[i++];
        }
    }
    return o == GBP_BAND_BYTES;
}

static void packet(gbp_t *p) {
    p->packets++;
    switch (p->cmd) {
    case 0x01:                                 /* INIT: a new picture */
        p->bands = 0;
        p->printed = false;
        p->palette = 0xE4;
        break;
    case 0x04:                                 /* DATA: a band, or the end of them */
        if (p->len) {
            uint8_t band[GBP_BAND_BYTES];
            if (band_bytes(p, band)) put_band(p, band);
            else p->bad_checksums++;           /* counted with the damaged */
        }
        break;
    case 0x02:                                 /* PRINT */
        if (p->len >= 4) {
            p->sheets = p->data[0];
            p->margins = p->data[1];
            p->palette = p->data[2] ? p->data[2] : 0xE4;   /* 00 means the default */
            p->exposure = p->data[3];
            p->printed = true;
            p->prints++;
        }
        break;
    default:                                   /* STATUS and the rest */
        break;
    }
}

void gbp_feed(gbp_t *p, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = b[i];
        switch (p->state) {
        case GBP_SEEK_88: if (c == 0x88) p->state = GBP_SEEK_33; break;
        case GBP_SEEK_33: p->state = c == 0x33 ? GBP_CMD : c == 0x88 ? GBP_SEEK_33 : GBP_SEEK_88; break;
        case GBP_CMD:  p->cmd = c;  p->sum = c; p->state = GBP_COMP; break;
        case GBP_COMP: p->comp = c; p->sum = (uint16_t)(p->sum + c); p->state = GBP_LEN0; break;
        case GBP_LEN0: p->len = c;  p->sum = (uint16_t)(p->sum + c); p->state = GBP_LEN1; break;
        case GBP_LEN1:
            p->len = (uint16_t)(p->len | (c << 8));
            p->sum = (uint16_t)(p->sum + c);
            p->got = 0;
            if (p->len > GBP_MAX_DATA) { p->bad_checksums++; p->state = GBP_SEEK_88; break; }
            p->state = p->len ? GBP_DATA : GBP_SUM0;
            break;
        case GBP_DATA:
            p->data[p->got++] = c;
            p->sum = (uint16_t)(p->sum + c);
            if (p->got == p->len) p->state = GBP_SUM0;
            break;
        case GBP_SUM0: p->want = c; p->state = GBP_SUM1; break;
        case GBP_SUM1:
            p->want = (uint16_t)(p->want | (c << 8));
            if (p->want == p->sum) packet(p);
            else p->bad_checksums++;
            p->state = GBP_SEEK_88;            /* the printer's 81 + status follow */
            break;
        }
    }
}
