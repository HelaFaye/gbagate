/*
 * mmo_link.c -- see mmo_link.h.
 */

#include "mmo_link.h"

#include <string.h>

#define CHECK_SEED 0x5A5Au

static uint16_t finish(uint16_t sum) {
    return sum == 0xFFFFu ? 0xFFFEu : sum;
}

int mmo_encode(const mmo_msg_t *m, uint16_t *out, int cap) {
    if (m->n > MMO_MAX_WORDS || m->type == 0 || m->type > 15 || cap < m->n + 2) {
        return 0;
    }
    uint16_t hdr = (uint16_t)(MMO_HDR | (m->type << 4) | m->n);
    uint16_t sum = (uint16_t)(CHECK_SEED + hdr);
    out[0] = hdr;
    for (int i = 0; i < m->n; i++) {
        if (m->w[i] == 0xFFFFu) {
            return 0;
        }
        out[1 + i] = m->w[i];
        sum = (uint16_t)(sum + m->w[i]);
    }
    out[1 + m->n] = finish(sum);
    return m->n + 2;
}

void mmo_rx_init(mmo_rx_t *r) {
    memset(r, 0, sizeof *r);
}

static bool is_header(uint16_t w) {
    return (w & 0xFF00u) == MMO_HDR && (w & 0xF0u) != 0;
}

bool mmo_rx_word(mmo_rx_t *r, uint16_t word) {
    if (r->have == 0) {
        if (word == MMO_IDLE || word == 0xFFFFu) {
            return false;                  /* between frames, or nobody there */
        }
        if (!is_header(word)) {
            r->bad++;                      /* a stray word: wait for a header */
            return false;
        }
        r->msg.type = (uint8_t)((word >> 4) & 0x0F);
        r->msg.n = (uint8_t)(word & 0x0F);
        r->sum = (uint16_t)(CHECK_SEED + word);
        r->have = 1;
        return false;
    }
    if (word == 0xFFFFu) {                 /* the partner went: not part of a frame */
        r->bad++;
        r->have = 0;
        return false;
    }
    if (r->have <= r->msg.n) {             /* payload */
        r->msg.w[r->have - 1] = word;
        r->sum = (uint16_t)(r->sum + word);
        r->have++;
        return false;
    }
    r->have = 0;                           /* the check */
    if (word != finish(r->sum)) {
        r->bad++;
        /* The bad word may itself start the next frame (a lost word shifts
         * everything by one): look at it again as a header. */
        if (is_header(word)) {
            return mmo_rx_word(r, word);
        }
        return false;
    }
    return true;
}
