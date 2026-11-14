/*
 * test_mpbus.c -- src/mp_bus.c, the board's side of a Multi-Player transfer
 * on the carrier's chain, against a model of GBAs on the wire.
 *
 * The model runs in virtual time, in nanoseconds. Each GBA does what
 * mp_bus.h says the wire does, and nothing the board tells it: the parent
 * drops SC and sends; a child sends a few microseconds after its SI goes
 * low, then drops its SO; every unit hears every word by its start bit and
 * samples mid-bit; SC rising ends the transfer. The parent GBA ends it when
 * a slot's start bit has not come within its timeout. The GBAs' turn delays
 * and the board's interrupt latency are random per transfer.
 *
 * What it checks, for every layout the chain carries: every GBA ends with
 * every slot's word, the board's rx is the same, nobody missed a turn, and
 * no two units ever drove SD at once.
 *
 * What it cannot check: the real GBA's turn delay and its parent timeout.
 * GBATEK and GB-Link's PIO programs give the shape, not those numbers, so
 * the model sweeps them; a scope on the carrier settles them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_bus.h"
#include "mp_link.h"

static int g_fail = 0;
static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-58s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* ---- the model ------------------------------------------------------------ */

#define BIT MPB_NS_BIT

typedef struct {
    int      port;           /* 1..3                                        */
    bool     parent;
    uint16_t word;           /* what it sends this transfer                 */
    uint32_t tok_delay;      /* SI low -> its start bit                     */
    /* state */
    bool     in_xfer, sent, sending, receiving;
    int      slot;           /* the next slot it will fill                  */
    uint16_t got[4];
    uint16_t rx_word;
    bool     sd_drive, sd_level, so;
    uint32_t wait_from;      /* parent: when the current slot's wait began  */
} gba_t;

typedef struct { uint32_t t; int g; int what; int arg; } ev_t;
enum { E_SEND_BIT, E_SAMPLE, E_SO_LOW, E_PARENT_CHECK, E_SC_RISE };

static struct {
    uint32_t vt;
    gba_t    g[3];
    int      n;              /* GBAs on the chain                           */
    bool     b_sd_drive, b_sd_level, b_so, b_sc_low;
    bool     sc_low_by_parent;
    ev_t     ev[512];
    int      nev;
    uint32_t contention, poll_cost, parent_timeout;
    bool     sd_prev, sc_prev;
} M;

static void push(uint32_t t, int g, int what, int arg) {
    if (M.nev < (int)(sizeof M.ev / sizeof M.ev[0])) M.ev[M.nev++] = (ev_t){ t, g, what, arg };
}

static bool sd_line(void) {
    int drivers = M.b_sd_drive;
    bool low = M.b_sd_drive && !M.b_sd_level;
    for (int i = 0; i < M.n; i++) {
        drivers += M.g[i].sd_drive;
        if (M.g[i].sd_drive && !M.g[i].sd_level) low = true;
    }
    if (drivers > 1) M.contention++;
    return !low;
}
static bool sc_line(void) { return !(M.b_sc_low || M.sc_low_by_parent); }
static bool si_of(const gba_t *g) { return g->port == 1 ? M.b_so : M.g[g->port - 2].so; }

static void gba_start_send(int i, uint32_t t) {
    gba_t *g = &M.g[i];
    g->sending = true;
    for (int b = 0; b <= 17; b++) push(t + (uint32_t)b * BIT, i, E_SEND_BIT, b);
    push(t + 18u * BIT, i, E_SEND_BIT, 18);   /* let go */
}

/* What the lines did since the last look: start bits, turns, the end. */
static void watch(void) {
    const bool sd = sd_line(), sc = sc_line();
    if (M.sd_prev && !sd) {                   /* a start bit */
        for (int i = 0; i < M.n; i++) {
            gba_t *g = &M.g[i];
            if (g->in_xfer && !g->sending && !g->receiving) {
                g->receiving = true;
                g->rx_word = 0;
                for (int b = 0; b < 16; b++) push(M.vt + BIT + BIT / 2 + (uint32_t)b * BIT, i, E_SAMPLE, b);
            }
        }
    }
    if (!M.sc_prev && sc) {                   /* the end */
        for (int i = 0; i < M.n; i++) {
            gba_t *g = &M.g[i];
            g->in_xfer = false;
            g->so = true;
        }
    }
    if (M.sc_prev && !sc) {                   /* a transfer begins */
        for (int i = 0; i < M.n; i++) {
            gba_t *g = &M.g[i];
            g->in_xfer = true;
            g->sent = g->receiving = false;
            g->slot = 0;
            for (int k = 0; k < 4; k++) g->got[k] = MPB_NO_WORD;
        }
    }
    M.sd_prev = sd;
    M.sc_prev = sc;
    /* children: the turn */
    for (int i = 0; i < M.n; i++) {
        gba_t *g = &M.g[i];
        if (!g->parent && g->in_xfer && !g->sent && !g->sending && !si_of(g)) {
            g->sent = true;                   /* committed */
            push(M.vt + g->tok_delay, i, E_SEND_BIT, -1);
        }
    }
}

static void apply(const ev_t *e) {
    gba_t *g = &M.g[e->g];
    switch (e->what) {
    case E_SEND_BIT:
        if (e->arg == -1) { gba_start_send(e->g, M.vt); return; }
        if (e->arg == 0) { g->sd_drive = true; g->sd_level = false; }
        else if (e->arg <= 16) g->sd_level = (g->word >> (e->arg - 1)) & 1u;
        else if (e->arg == 17) g->sd_level = true;
        else {
            g->sd_drive = false;
            g->sending = false;
            g->got[g->slot++ & 3] = g->word;
            push(M.vt, e->g, E_SO_LOW, 0);
            if (g->parent) { g->wait_from = M.vt; push(M.vt + M.parent_timeout, e->g, E_PARENT_CHECK, g->slot); }
        }
        break;
    case E_SAMPLE:
        if (sd_line()) g->rx_word |= (uint16_t)(1u << e->arg);
        if (e->arg == 15) {
            g->receiving = false;
            if (g->in_xfer) g->got[g->slot++ & 3] = g->rx_word;
            if (g->parent) {
                if (g->slot >= 4) push(M.vt + 2 * BIT, e->g, E_SC_RISE, 0);
                else { g->wait_from = M.vt; push(M.vt + 2 * BIT + M.parent_timeout, e->g, E_PARENT_CHECK, g->slot); }
            }
        }
        break;
    case E_SO_LOW:
        g->so = false;
        break;
    case E_PARENT_CHECK:
        /* nothing started in the slot it was waiting for: end the transfer */
        if (g->in_xfer && g->slot == e->arg && !g->receiving) push(M.vt + MPB_NS_MASTER_HOLD, e->g, E_SC_RISE, 0);
        break;
    case E_SC_RISE:
        M.sc_low_by_parent = false;
        break;
    }
}

static void advance(uint32_t t) {
    for (;;) {
        int best = -1;
        for (int i = 0; i < M.nev; i++)
            if ((int32_t)(M.ev[i].t - t) <= 0 && (best < 0 || (int32_t)(M.ev[i].t - M.ev[best].t) < 0)) best = i;
        if (best < 0) break;
        const ev_t e = M.ev[best];
        M.ev[best] = M.ev[--M.nev];
        if ((int32_t)(e.t - M.vt) > 0) M.vt = e.t;
        apply(&e);
        watch();
    }
    if ((int32_t)(t - M.vt) > 0) M.vt = t;
    watch();
}

/* ---- the board's HAL over the model --------------------------------------- */

static uint32_t h_now(void *c) { (void)c; advance(M.vt + M.poll_cost); return M.vt; }
static void h_wait(void *c, uint32_t t) { (void)c; if ((int32_t)(t - M.vt) > 0) advance(t); else advance(M.vt + M.poll_cost); }
static bool h_sd(void *c) { (void)c; advance(M.vt + M.poll_cost); return sd_line(); }
static void h_sd_out(void *c, bool drive, bool level) { (void)c; M.b_sd_drive = drive; M.b_sd_level = level; watch(); }
static void h_sc(void *c, bool low) { (void)c; M.b_sc_low = low; watch(); }
static void h_so(void *c, bool high) { (void)c; M.b_so = high; watch(); }
static bool h_si(void *c, int port) { (void)c; advance(M.vt + M.poll_cost); return M.g[port - 1].so; }

static const mp_bus_hal_t HAL = { h_now, h_wait, h_sd, h_sd_out, h_sc, h_so, h_si, NULL, 1000 };

/* ---- running transfers ------------------------------------------------------ */

static uint32_t rnd(uint32_t lo, uint32_t hi) { return lo + (uint32_t)rand() % (hi - lo + 1); }


typedef struct { uint32_t transfers, bad_words, contention, missed, late; } result_t;

static void model_setup(const mp_bus_layout_t *l, int present) {
    memset(&M, 0, sizeof M);
    M.n = present;
    M.poll_cost = 60;
    M.parent_timeout = MPB_NS_SLOT_TIMEOUT + 20000;
    M.vt = 0xFFFFFFFFu - 2000000u;            /* the clock wraps 2 ms in: every run crosses it */
    M.sd_prev = M.sc_prev = true;
    for (int i = 0; i < M.n; i++) {
        M.g[i].port = i + 1;
        M.g[i].parent = !mp_bus_is_parent(l) && i == 0;
        M.g[i].so = true;
    }
    mp_bus_idle(&HAL, l);
}

/* One transfer with the GBAs' words given; the board's from the relay
 * through mp_link, as the driver runs it. */
static void link_transfer(mp_link_t *m, const uint16_t gba[3]) {
    const mp_bus_layout_t *l = &m->layout;
    for (int i = 0; i < M.n; i++) {
        M.g[i].word = gba[i];
        M.g[i].tok_delay = rnd(1000, 12000);
    }
    uint16_t tx[4], rx[4];
    uint32_t gap = 0;
    mp_link_next(m, tx, &gap);
    if (mp_bus_is_parent(l)) {
        mp_bus_parent(&HAL, l, tx, rx, NULL);
    } else {
        M.sc_low_by_parent = true;
        gba_start_send(0, M.vt);
        watch();
        const uint32_t t_fall = M.vt;
        advance(M.vt + rnd(600, 1500));
        mp_bus_child(&HAL, l, t_fall - 1000, tx, rx, NULL);
    }
    advance(M.vt + 400000);
    mp_link_done(m, rx);
    advance(M.vt + (mp_bus_is_parent(l) ? gap * 1000u : 1000000u));
}

/* Three GBAs and the board on the chain, through the games' own handshake
 * and a frame: what the relay answers, as the GBAs hear it. */
static void test_with_the_relay(void) {
    static mp_link_t m;
    const mp_bus_layout_t l = { 4, 3, 0 };
    mp_link_init(&m, &l, NULL, NULL, NULL);
    model_setup(&l, 3);
    check("the relay's local slots are the chain's: 0-2", m.relay.local_mask, 0x7);

    const uint16_t hs[3] = { MP_SLAVE_HANDSHAKE, MP_SLAVE_HANDSHAKE, MP_SLAVE_HANDSHAKE };
    for (int i = 0; i < 3; i++) link_transfer(&m, hs);
    check("before the session: three GBAs seen on the chain", mp_detect_count(&m.relay), 3);
    check("... and slot 3 is nobody's", M.g[0].got[3], MPB_NO_WORD);

    mp_start_handshake(&m.relay);
    link_transfer(&m, hs);
    check("after StartHandshake every GBA hears B9A0 in slot 3",
          M.g[0].got[3] == MP_SLAVE_HANDSHAKE && M.g[1].got[3] == MP_SLAVE_HANDSHAKE &&
          M.g[2].got[3] == MP_SLAVE_HANDSHAKE, 1);

    const uint16_t go[3] = { MP_MASTER_HANDSHAKE, MP_SLAVE_HANDSHAKE, MP_SLAVE_HANDSHAKE };
    link_transfer(&m, go);
    link_transfer(&m, hs);                         /* the checksum slot */
    check("the parent's 8FFF connects: frames", m.relay.phase, MP_FRAMES);

    const uint16_t remote[8] = { 0xBBBB, 0x003C, 0x0083, 0, 0, 0, 0, 0 };
    mp_push(&m.relay, 3, remote);
    uint32_t sum = 0;
    for (int k = 0; k < 8; k++) {                  /* a frame of idle words: the queue is taken at its start */
        const uint16_t w[3] = { (uint16_t)(k == 0 ? 0x1111 : 0), 0, (uint16_t)(k == 0 ? 0x3333 : 0) };
        link_transfer(&m, w);
        for (int s = 0; s < 4; s++) sum += M.g[0].got[s];
    }
    const uint16_t c[3] = { (uint16_t)sum, (uint16_t)sum, (uint16_t)sum };
    link_transfer(&m, c);
    check("the board's checksum word, as the GBAs hear it, is the frame's sum", M.g[1].got[3], (uint16_t)sum);
    check("no GBA's checksum disagreed with the relay's", m.relay.bad_checksums, 0);
    uint16_t cmd[8];
    uint8_t from;
    check("port 1's command goes to the network as slot 0's", mp_pop(&m.relay, &from, cmd) && from == 0 && cmd[0] == 0x1111, 1);
    check("port 3's as slot 2's", mp_pop(&m.relay, &from, cmd) && from == 2 && cmd[0] == 0x3333, 1);
    link_transfer(&m, hs);
    check("the remote player's command reaches every GBA in slot 3", M.g[2].got[3], 0xBBBB);
    check("SD never driven twice", M.contention, 0);
}

/* `present`: GBAs actually plugged in (a test can say fewer than the layout). */
static result_t run(const mp_bus_layout_t *l, int present, int transfers, uint32_t tok_lo, uint32_t tok_hi,
                    uint32_t isr_lo, uint32_t isr_hi, int32_t est_err) {
    result_t r = { 0, 0, 0, 0, 0 };
    model_setup(l, present);
    mp_bus_stats_t st = { 0, 0, 0 };
    for (int x = 0; x < transfers; x++) {
        uint16_t want[4] = { MPB_NO_WORD, MPB_NO_WORD, MPB_NO_WORD, MPB_NO_WORD }, tx[4] = { 0 }, rx[4];
        for (int k = 0; k < l->players; k++) {
            want[k] = (uint16_t)rand();
            const int port = mp_bus_port_of(l, k);
            if (port && port <= M.n) M.g[port - 1].word = want[k];
            else if (!port) tx[k] = want[k];
        }
        for (int i = 0; i < M.n; i++) M.g[i].tok_delay = rnd(tok_lo, tok_hi);
        const uint32_t c0 = M.contention;

        if (mp_bus_is_parent(l)) {
            mp_bus_parent(&HAL, l, tx, rx, &st);
        } else {
            /* the parent GBA starts; the board's interrupt runs a little after */
            M.sc_low_by_parent = true;
            gba_start_send(0, M.vt);
            watch();
            const uint32_t t_fall = M.vt;
            advance(M.vt + rnd(isr_lo, isr_hi));
            mp_bus_child(&HAL, l, (uint32_t)((int32_t)t_fall + est_err), tx, rx, &st);
        }
        advance(M.vt + 400000);                    /* let the transfer finish */

        int bad = 0;
        for (int i = 0; i < M.n; i++)
            for (int k = 0; k < l->players; k++)
                if (M.g[i].got[k] != want[k]) bad++;
        for (int k = 0; k < l->players; k++)
            if (rx[k] != want[k]) bad++;
        r.bad_words += (uint32_t)bad;
        r.contention += M.contention - c0;
        r.transfers++;
        advance(M.vt + rnd(500000, 2000000));      /* the games' gap between transfers */
    }
    r.missed = st.missed_slots;
    r.late = st.late_turns;
    return r;
}

static void layout(int players, int locals, int first) {
    char what[96];
    const mp_bus_layout_t l = { (uint8_t)players, (uint8_t)locals, (uint8_t)first };
    snprintf(what, sizeof what, "%dP, %d on the chain from slot %d", players, locals, first);
    /* turn delays 1-12 us, interrupt latency 0.6-1.5 us, estimated as 1 us */
    result_t r = run(&l, locals, 300, 1000, 12000, 600, 1500, -1000);
    char w[160];
    snprintf(w, sizeof w, "%s: every word, everywhere", what);   check(w, r.bad_words, 0);
    snprintf(w, sizeof w, "%s: SD never driven twice", what);     check(w, r.contention, 0);
    snprintf(w, sizeof w, "%s: no slot missed, no turn late", what); check(w, r.missed + r.late, 0);
}

int main(void) {
    srand(1234);

    /* the layouts */
    const mp_bus_layout_t ok[] = { { 2, 1, 0 }, { 2, 1, 1 }, { 4, 3, 0 }, { 4, 1, 1 }, { 4, 1, 3 }, { 4, 3, 1 } };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) check("layouts the chain carries", mp_bus_layout_ok(&ok[i]), 1);
    const mp_bus_layout_t bad[] = { { 4, 4, 0 }, { 1, 1, 0 }, { 5, 3, 0 }, { 3, 0, 1 }, { 4, 2, 3 }, { 3, 1, 3 } };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) check("and ones it cannot", mp_bus_layout_ok(&bad[i]), 0);
    const mp_bus_layout_t a = { 4, 3, 0 }, b = { 4, 1, 1 }, c = { 3, 2, 1 }, d = { 4, 1, 3 };
    check("3 GBAs, GBA parent: the board is slot 3", mp_bus_board_slots(&a), 0x8);
    check("1 GBA, board parent: slots 0, 2, 3", mp_bus_board_slots(&b), 0xD);
    check("... the GBA is slot 1, on port 1", mp_bus_port_of(&b, 1), 1);
    check("2 GBAs of 3, board parent: slot 0 only", mp_bus_board_slots(&c), 0x1);
    check("1 GBA, the fourth player: the board is 0-2", mp_bus_board_slots(&d), 0x7);
    check("... and its GBA is slot 3, on port 1", mp_bus_port_of(&d, 3), 1);

    /* every layout */
    for (int players = 2; players <= 4; players++)
        for (int locals = 1; locals < players && locals <= 3; locals++)
            for (int first = 0; first + locals <= players; first++)
                layout(players, locals, first);

    /* the margins: an interrupt estimate 2 us off either way, slow GBAs */
    const mp_bus_layout_t four = { 4, 1, 0 };
    result_t r = run(&four, 1, 200, 1000, 12000, 600, 1500, -3000);
    check("the edge estimated 2 us early: still every word", r.bad_words + r.contention, 0);
    r = run(&four, 1, 200, 1000, 12000, 600, 1500, +1000);
    check("... 2 us late", r.bad_words + r.contention, 0);
    const mp_bus_layout_t chain = { 4, 3, 1 };
    r = run(&chain, 3, 200, 30000, 60000, 600, 1500, -1000);
    check("GBAs slow to take their turn (30-60 us)", r.bad_words + r.contention + r.missed + r.late, 0);

    test_with_the_relay();

    /* a GBA missing from the chain: the board gives up on that turn, never
     * drives over anyone, and the next transfer still goes */
    const mp_bus_layout_t gone = { 4, 3, 0 };
    r = run(&gone, 2, 20, 1000, 12000, 600, 1500, -1000);
    check("port 3 empty: its slot is missed", r.missed > 0 || r.late > 0, 1);
    check("... and nothing drives SD twice", r.contention, 0);

    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
