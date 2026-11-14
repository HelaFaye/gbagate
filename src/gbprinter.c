/*
 * gbprinter.c -- see gbprinter.h.
 */
#include "gbprinter.h"

#include <string.h>

void gbp_init(gbp_t *p) {
    memset(p, 0, sizeof *p);
    p->palette = 0xE4;                         /* colour c -> shade c */
}

/* Drop the picture: the next band is its first row. */
static void new_picture(gbp_t *p) {
    p->bands = 0;
    p->committed = 0;
    p->finished = false;
}

/* One band of 20 x 2 tiles, 16 bytes a tile (two bitplanes a row). */
static void put_band(gbp_t *p, const uint8_t *band) {
    if (p->finished) new_picture(p);
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

/* The paper feeds out: what was printed is the picture. */
static bool finish(gbp_t *p) {
    if (p->finished || p->committed == 0) return false;
    p->bands = (uint8_t)(p->committed / GBP_BAND_ROWS);   /* unprinted bands go */
    p->finished = true;
    p->pictures++;
    return true;
}

bool gbp_finish(gbp_t *p) { return finish(p); }

static void print(gbp_t *p) {
    p->sheets = p->data[0];
    p->margins = p->data[1];
    p->palette = p->data[2] ? p->data[2] : 0xE4;          /* 00 means the default */
    p->exposure = p->data[3];
    p->printed = true;
    p->prints++;
    if (p->finished) return;                  /* nothing new to print */
    for (int r = p->committed; r < gbp_rows(p); r++)
        for (int x = 0; x < GBP_WIDTH; x++)
            p->pixels[r][x] = (uint8_t)((p->palette >> (2 * p->pixels[r][x])) & 3);
    p->committed = (uint16_t)gbp_rows(p);
    if (p->margins & 0x0F) finish(p);          /* a margin after: the paper feeds */
}

static void packet(gbp_t *p) {
    p->packets++;
    switch (p->cmd) {
    case 0x01:                                 /* INIT */
        if (p->finished) new_picture(p);
        else p->bands = (uint8_t)(p->committed / GBP_BAND_ROWS);   /* the unprinted go */
        break;
    case 0x04:                                 /* DATA: a band, or the end of them */
        if (p->len) {
            uint8_t band[GBP_BAND_BYTES];
            if (band_bytes(p, band)) put_band(p, band);
            else p->bad_checksums++;           /* counted with the damaged */
        }
        break;
    case 0x02:                                 /* PRINT */
        if (p->len >= 4) print(p);
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

/* gbp_feed_stream's states */
enum { S_PACKET, S_COMP, S_LEN0, S_LEN1, S_DATA, S_ABORT };
static const char k_abort[] = "ABORTPRINT";

void gbp_feed_stream(gbp_t *p, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = b[i];
        switch (p->sstate) {
        case S_PACKET:                         /* between packets: a command or a marker */
            if (c == 0xFF || c == 0xFE) break; /* the mode started; a PRINT was answered */
            if (c == (uint8_t)k_abort[0]) { p->sstate = S_ABORT; p->abort_at = 1; break; }
            p->cmd = c;
            p->sstate = S_COMP;
            break;
        case S_COMP: p->comp = c; p->sstate = S_LEN0; break;
        case S_LEN0: p->len = c;  p->sstate = S_LEN1; break;
        case S_LEN1:
            p->len = (uint16_t)(p->len | (c << 8));
            p->got = 0;
            if (p->len > GBP_MAX_DATA) {       /* not a packet: lost our place */
                p->bad_checksums++;
                p->sstate = S_PACKET;
            } else if (p->len) {
                p->sstate = S_DATA;
            } else {
                packet(p);
                p->sstate = S_PACKET;
            }
            break;
        case S_DATA:
            p->data[p->got++] = c;
            if (p->got == p->len) { packet(p); p->sstate = S_PACKET; }
            break;
        case S_ABORT:
            if (c != (uint8_t)k_abort[p->abort_at]) { p->sstate = S_PACKET; break; }
            if (++p->abort_at == sizeof k_abort - 1) { p->aborts++; p->sstate = S_PACKET; }
            break;
        }
    }
}
