/*
 * mp_link.c -- see mp_link.h.
 */
#include "mp_link.h"

#include <string.h>

void mp_link_init(mp_link_t *m, const mp_bus_layout_t *l,
                  void (*lock)(void *), void (*unlock)(void *), void *lock_ctx) {
    memset(m, 0, sizeof *m);
    m->layout = *l;
    m->lock = lock;
    m->unlock = unlock;
    m->lock_ctx = lock_ctx;
    const uint8_t all = (uint8_t)((1u << l->players) - 1u);
    mp_init(&m->relay, l->players, (uint8_t)(all & ~mp_bus_board_slots(l)));
}

static void take(mp_link_t *m) { if (m->lock) m->lock(m->lock_ctx); }
static void give(mp_link_t *m) { if (m->unlock) m->unlock(m->lock_ctx); }

uint32_t mp_link_gap_us(const mp_relay_t *r) {
    if (r->phase != MP_FRAMES) return MP_LINK_GAP_HANDSHAKE_US;
    return r->pos == 0 ? MP_LINK_GAP_FRAME_US : MP_LINK_GAP_WORD_US;
}

void mp_link_next(void *ctx, uint16_t tx[4], uint32_t *gap_us) {
    mp_link_t *m = ctx;
    uint16_t w[MP_MAX_PLAYERS] = { 0 };
    take(m);
    mp_tx(&m->relay, w);
    if (gap_us) *gap_us = mp_link_gap_us(&m->relay);
    give(m);
    for (int k = 0; k < 4; k++) tx[k] = w[k];
}

void mp_link_done(void *ctx, const uint16_t rx[4]) {
    mp_link_t *m = ctx;
    uint16_t w[MP_MAX_PLAYERS];
    for (int k = 0; k < 4; k++) w[k] = rx[k];
    take(m);
    mp_done(&m->relay, w);
    give(m);
}
