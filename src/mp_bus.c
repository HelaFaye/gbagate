/*
 * mp_bus.c -- see mp_bus.h.
 */
#include "mp_bus.h"

bool mp_bus_layout_ok(const mp_bus_layout_t *l) {
    return l->players >= 2 && l->players <= 4 &&
           l->locals >= 1 && l->locals <= MPB_MAX_PORTS && l->locals < l->players;
}

uint8_t mp_bus_board_slots(const mp_bus_layout_t *l) {
    uint8_t m = 0;
    for (int s = 0; s < l->players; s++)
        if (!mp_bus_port_of(l, s)) m |= (uint8_t)(1u << s);
    return m;
}

int mp_bus_port_of(const mp_bus_layout_t *l, int slot) {
    if (l->parent) return slot >= 1 && slot <= l->locals ? slot : 0;
    return slot < l->locals ? slot + 1 : 0;
}

static bool before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

/* ns -> the HAL's ticks */
static uint32_t T(const mp_bus_hal_t *h, uint32_t ns) {
    return (uint32_t)(((uint64_t)ns * h->ticks_per_us + 500u) / 1000u);
}

/* Start bit, 16 bits LSB first, stop bit, from t; then let SD go. Returns
 * the time the stop bit ends. */
static uint32_t send_word(const mp_bus_hal_t *h, uint16_t w, uint32_t t) {
    h->wait_until(h->ctx, t);
    h->sd_out(h->ctx, true, false);
    for (int i = 0; i < 16; i++) {
        t += T(h, MPB_NS_BIT);
        h->wait_until(h->ctx, t);
        h->sd_out(h->ctx, true, (w >> i) & 1u);
    }
    t += T(h, MPB_NS_BIT);
    h->wait_until(h->ctx, t);
    h->sd_out(h->ctx, true, true);
    t += T(h, MPB_NS_BIT);
    h->wait_until(h->ctx, t);
    h->sd_out(h->ctx, false, true);
    return t;
}

/* 16 bits, the first sampled at t. */
static uint16_t recv_bits(const mp_bus_hal_t *h, uint32_t t) {
    uint16_t w = 0;
    for (int i = 0; i < 16; i++) {
        h->wait_until(h->ctx, t);
        if (h->sd_get(h->ctx)) w |= (uint16_t)(1u << i);
        t += T(h, MPB_NS_BIT);
    }
    return w;
}

/* Poll for a start bit (SD low) until deadline; its time in *te. */
static bool wait_start(const mp_bus_hal_t *h, uint32_t deadline, uint32_t *te) {
    for (;;) {
        const uint32_t t = h->now(h->ctx);
        if (!h->sd_get(h->ctx)) { *te = t; return true; }
        if (!before(t, deadline)) return false;
    }
}

/* Poll for a port's GBA passing the turn (its SO low) until deadline. */
static bool wait_turn(const mp_bus_hal_t *h, int port, uint32_t deadline, uint32_t *tt) {
    for (;;) {
        const uint32_t t = h->now(h->ctx);
        if (!h->si_get(h->ctx, port)) { *tt = t; return true; }
        if (!before(t, deadline)) return false;
    }
}

/*
 * Slots from `first` on, after the previous unit's word ended at *t_end;
 * prev_port is that unit's port (0: the board). Shared by both roles:
 * after slot 0 the chain behaves the same whoever is parent.
 */
static void rest_of_transfer(const mp_bus_hal_t *h, const mp_bus_layout_t *l, int first,
                             int prev_port, uint32_t t_end, const uint16_t tx[4], uint16_t rx[4],
                             mp_bus_stats_t *st) {
    for (int k = first; k < l->players; k++) {
        const int port = mp_bus_port_of(l, k);
        if (!port) {                                   /* ours */
            uint32_t t;
            if (!prev_port) {
                t = t_end + T(h, MPB_NS_BACK_TO_BACK);       /* our own previous slot */
            } else {
                uint32_t tt;
                if (!wait_turn(h, prev_port, t_end + T(h, MPB_NS_SLOT_TIMEOUT), &tt)) {
                    if (st) st->late_turns++;          /* the chain stalled */
                    return;
                }
                t = tt + T(h, MPB_NS_TURN);
            }
            t_end = send_word(h, tx[k], t);
            rx[k] = tx[k];
        } else {                                       /* a GBA on the chain */
            uint32_t te;
            if (!wait_start(h, t_end + T(h, MPB_NS_SLOT_TIMEOUT), &te)) {
                if (st) st->missed_slots++;
                return;                                /* it holds the turn: the rest cannot go */
            }
            rx[k] = recv_bits(h, te + T(h, MPB_NS_START_TO_MID));
            /* past the last data bit before looking for anything else */
            h->wait_until(h->ctx, te + T(h, 17u * MPB_NS_BIT + MPB_NS_BIT / 2));
            t_end = te + T(h, MPB_NS_WORD);
        }
        prev_port = port;
    }
}

void mp_bus_child(const mp_bus_hal_t *h, const mp_bus_layout_t *l, uint32_t t_sc_fall,
                  const uint16_t tx[4], uint16_t rx[4], mp_bus_stats_t *st) {
    for (int k = 0; k < 4; k++) rx[k] = MPB_NO_WORD;
    rx[0] = recv_bits(h, t_sc_fall + T(h, MPB_NS_SAMPLE0));   /* port 1's GBA, the parent */
    h->wait_until(h->ctx, t_sc_fall + T(h, 17u * MPB_NS_BIT + MPB_NS_BIT / 2));
    rest_of_transfer(h, l, 1, 1, t_sc_fall + T(h, MPB_NS_WORD), tx, rx, st);
    if (st) st->transfers++;
}

void mp_bus_parent(const mp_bus_hal_t *h, const mp_bus_layout_t *l,
                   const uint16_t tx[4], uint16_t rx[4], mp_bus_stats_t *st) {
    for (int k = 0; k < 4; k++) rx[k] = MPB_NO_WORD;
    const uint32_t t0 = h->now(h->ctx);
    h->sc_out(h->ctx, true);                           /* busy */
    const uint32_t t_end = send_word(h, tx[0], t0);
    rx[0] = tx[0];
    h->so_out(h->ctx, false);                          /* port 1's turn */
    rest_of_transfer(h, l, 1, 0, t_end, tx, rx, st);
    h->so_out(h->ctx, true);
    h->wait_until(h->ctx, h->now(h->ctx) + T(h, MPB_NS_MASTER_HOLD));
    h->sc_out(h->ctx, false);                          /* idle */
    if (st) st->transfers++;
}

void mp_bus_idle(const mp_bus_hal_t *h, const mp_bus_layout_t *l) {
    h->sd_out(h->ctx, false, true);
    if (l->parent) {
        h->so_out(h->ctx, true);
        h->sc_out(h->ctx, false);
    } else {
        h->so_out(h->ctx, false);
    }
}
