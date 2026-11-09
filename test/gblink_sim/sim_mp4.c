/*
 * sim_mp4.c -- the board's Multi-Play bus in the simulator, for a Celio
 * multi session (src/mp_online.c run by port/gblink/celio_glue.cpp).
 *
 * On the board the link driver runs the bus (src/mp_bus.c) against real
 * GBAs; test/test_mpbus.c checks that layer against modelled ones. Here
 * the bus is one level up: each transfer is the four slots' words, and the
 * GBAs on the chain are modelled at the games' protocol -- the handshake,
 * 8FFF from the parent, the unchecked slot, frames of eight words and a
 * shared checksum that each GBA checks for every player -- so that the
 * board's Celio client, the glue and the relay run for real against
 * Celio-Server (test/gblink_compat/test_celio_mp4.mjs).
 *
 * Each GBA sends one command once in frames, [C0DE, slot, 1234 + slot, 0..]:
 * what every other GBA must end up hearing.
 */
#include <stdatomic.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "gblink_port.h"
#include "mp_link.h"
#include "sim.h"
#include "zshim.h"

typedef struct {
    int      slot;
    bool     parent, sent_cmd;
    int      phase, pos, ready, frames;
    uint16_t sum, mine[8], frame[4][8];
    uint16_t checksum_errors;
    uint8_t  got_mask;           /* slots whose command it has heard */
} gba_t;

static mp_link_t *volatile s_link;
static mp_bus_layout_t     s_layout;
static gba_t               s_gba[3];
static atomic_bool         s_on;
static atomic_uint         s_transfers;
static bool                s_thread;

static uint16_t sig(int slot, int k) {
    static const uint16_t base[8] = { 0xC0DE, 0, 0x1234, 0, 0, 0, 0, 0 };
    return k == 1 ? (uint16_t)slot : k == 2 ? (uint16_t)(base[2] + slot) : base[k];
}

static uint16_t gba_word(gba_t *g) {
    if (g->phase == 0) return g->parent && g->ready >= 3 ? 0x8FFF : 0xB9A0;
    if (g->phase == 1) return 0xB9A0;
    if (g->pos == 0) {
        memset(g->mine, 0, sizeof g->mine);
        if (!g->sent_cmd && g->frames >= 2) {
            for (int k = 0; k < 8; k++) g->mine[k] = sig(g->slot, k);
            g->sent_cmd = true;
        }
    }
    return g->pos < 8 ? g->mine[g->pos] : g->sum;
}

static void gba_after(gba_t *g, const uint16_t w[4], int players) {
    if (g->phase == 0) {
        bool all = true;
        for (int s = 0; s < players; s++) all = all && (w[s] == 0xB9A0 || w[s] == 0x8FFF);
        g->ready = all ? g->ready + 1 : 0;
        if (w[0] == 0x8FFF && all) g->phase = 1;
        return;
    }
    if (g->phase == 1) { g->phase = 2; g->pos = 0; g->sum = 0; return; }
    if (g->pos < 8) {
        for (int s = 0; s < players; s++) { g->sum = (uint16_t)(g->sum + w[s]); g->frame[s][g->pos] = w[s]; }
        g->pos++;
        return;
    }
    for (int s = 0; s < players; s++) {
        if (w[s] != g->sum) g->checksum_errors++;
        bool is_sig = true;
        for (int k = 0; k < 8; k++) is_sig = is_sig && g->frame[s][k] == sig(s, k);
        if (is_sig) g->got_mask |= (uint8_t)(1u << s);
    }
    g->frames++;
    g->pos = 0;
    g->sum = 0;
}

static void cable(void *, void *, void *) {
    for (;;) {
        k_sleep(K_MSEC(2));
        if (!atomic_load(&s_on)) continue;
        mp_link_t *link = s_link;
        uint16_t w[4];
        uint32_t gap;
        mp_link_next(link, w, &gap);
        for (int i = 0; i < s_layout.locals; i++) w[s_gba[i].slot] = gba_word(&s_gba[i]);
        for (int i = 0; i < s_layout.locals; i++) gba_after(&s_gba[i], w, s_layout.players);
        mp_link_done(link, w);
        atomic_fetch_add(&s_transfers, 1);
    }
}

bool gblink_mp4_bus_start(const mp_bus_layout_t *l, mp_link_t *link) {
    atomic_store(&s_on, false);
    k_sleep(K_MSEC(5));                       /* let a transfer in progress end */
    s_layout = *l;
    s_link = link;
    memset(s_gba, 0, sizeof s_gba);
    for (int i = 0; i < l->locals; i++) {
        s_gba[i].slot = l->first + i;
        s_gba[i].parent = l->first == 0 && i == 0;
    }
    atomic_store(&s_transfers, 0);
    if (!s_thread) {
        s_thread = true;
        zshim_thread_start("sim_mp4", cable, 8192, 2);
    }
    atomic_store(&s_on, true);
    return true;
}

void gblink_mp4_bus_stop(void) { atomic_store(&s_on, false); }

/* SIM_EV_MP4: [on, players, locals, first, u32 transfers,
 *              then per GBA: phase, got_mask, u16 checksum_errors] */
void sim_mp4_report(void) {
    uint8_t b[8 + 3 * 4];
    b[0] = (uint8_t)atomic_load(&s_on);
    b[1] = s_layout.players;
    b[2] = s_layout.locals;
    b[3] = s_layout.first;
    const uint32_t t = atomic_load(&s_transfers);
    memcpy(b + 4, &t, 4);
    for (int i = 0; i < 3; i++) {
        b[8 + 4 * i] = (uint8_t)s_gba[i].phase;
        b[9 + 4 * i] = s_gba[i].got_mask;
        memcpy(b + 10 + 4 * i, &s_gba[i].checksum_errors, 2);
    }
    sim_event(SIM_EV_MP4, b, sizeof b);
}
