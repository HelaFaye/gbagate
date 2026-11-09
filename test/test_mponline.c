/*
 * test_mponline.c -- src/mp_online.c: two boards in one Multi-Play link
 * over Celio, the server played by the test.
 *
 * Board A has three GBAs on its chain, board B one: four players. On each
 * board's cable the GBAs are simulated at the games' protocol level (the
 * handshake, 8FFF from the parent, the unchecked slot, frames of eight
 * words and a shared checksum, each GBA checking every player's), and the
 * board's side is the relay through mp_link, as the link driver runs it.
 * The server does what backport/Celio-Link-Server/0002 does: roles by link
 * ID, StartHandshake once every board has reported HandshakeReceived,
 * ConnectLink to the others when one reports LinkConnected, and data to the
 * others marked with the sender's ID.
 */
#include <stdio.h>
#include <string.h>

#include "mp_online.h"

static int g_fail = 0;
static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-58s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* ---- a GBA, as the games' link code behaves ------------------------------- */

typedef struct {
    int      slot;
    bool     parent;
    int      phase;          /* 0 handshake, 1 the unchecked slot, 2 frames */
    int      pos, ready;
    uint16_t sum, mine[8];
    bool     have_cmd;
    uint16_t cmd[8];         /* what it sends in its next frame */
    int      checksum_errors;
    uint16_t got[4][8];      /* each player's last non-idle command */
    uint16_t frame[4][8];
} gba_t;

static uint16_t gba_word(gba_t *g) {
    if (g->phase == 0) return g->parent && g->ready >= 3 ? 0x8FFF : 0xB9A0;
    if (g->phase == 1) return 0xB9A0;
    if (g->pos == 0) {       /* a frame starts: this GBA's command for it */
        memset(g->mine, 0, sizeof g->mine);
        if (g->have_cmd) { memcpy(g->mine, g->cmd, sizeof g->mine); g->have_cmd = false; }
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
        bool any = false;
        for (int k = 0; k < 8; k++) any = any || g->frame[s][k];
        if (any) memcpy(g->got[s], g->frame[s], sizeof g->got[s]);
    }
    g->pos = 0;
    g->sum = 0;
}

/* ---- a board ------------------------------------------------------------------ */

typedef struct board {
    mp_online_t    o;
    mp_bus_layout_t layout;
    mp_link_t     *link;
    bool           bus_on;
    gba_t          gba[3];
    int            ngba;
    uint16_t       statuses[8];
    int            nstatus;
    int            id;           /* the link ID the server gave it */
} board_t;

static board_t A, B;
static board_t *other(board_t *b) { return b == &A ? &B : &A; }

/* the network: what each board sent, for the other, with the sender's ID */
static struct { board_t *to; uint8_t from; uint8_t data[64]; } g_net[256];
static int g_nnet;

static bool bus_start(void *ctx, const mp_bus_layout_t *l, mp_link_t *link) {
    board_t *b = ctx;
    b->layout = *l;
    b->link = link;
    b->bus_on = true;
    return true;
}
static void bus_stop(void *ctx) { ((board_t *)ctx)->bus_on = false; }
static void status(void *ctx, uint16_t s) {
    board_t *b = ctx;
    if (b->nstatus < 8) b->statuses[b->nstatus++] = s;
}
static void data_out(void *ctx, const uint8_t d[64]) {
    board_t *b = ctx;
    if (g_nnet < 256) {
        g_net[g_nnet].to = other(b);
        g_net[g_nnet].from = (uint8_t)b->id;
        memcpy(g_net[g_nnet].data, d, 64);
        g_nnet++;
    }
}

static void board_init(board_t *b, int gbas) {
    memset(b, 0, sizeof *b);
    const mp_online_io_t io = { bus_start, bus_stop, status, data_out, NULL, NULL, b };
    mp_online_init(&b->o, &io, 4, (uint8_t)gbas);
    b->ngba = gbas;
}

/* One transfer on a board's cable. */
static void cable(board_t *b) {
    uint16_t w[4];
    uint32_t gap;
    mp_link_next(b->link, w, &gap);
    for (int i = 0; i < b->ngba; i++) w[b->gba[i].slot] = gba_word(&b->gba[i]);
    for (int i = 0; i < b->ngba; i++) gba_after(&b->gba[i], w, 4);
    mp_link_done(b->link, w);
}

/* The server: what it does with each board's statuses. */
static bool g_hs[2], g_started;
static void server(void) {
    board_t *bs[2] = { &A, &B };
    for (int i = 0; i < 2; i++) {
        board_t *b = bs[i];
        for (int k = 0; k < b->nstatus; k++) {
            if (b->statuses[k] == MPO_ST_HANDSHAKE_RECEIVED) g_hs[i] = true;
            if (b->statuses[k] == MPO_ST_LINK_CONNECTED)
                mp_online_command(&other(b)->o, 4, MPO_CMD_CONNECT_LINK, other(b)->id);
        }
        b->nstatus = 0;
    }
    if (g_hs[0] && g_hs[1] && !g_started) {
        g_started = true;
        mp_online_command(&A.o, 4, MPO_CMD_START_HANDSHAKE, A.id);
        mp_online_command(&B.o, 4, MPO_CMD_START_HANDSHAKE, B.id);
    }
    for (int n = 0; n < g_nnet; n++) mp_online_data(&g_net[n].to->o, g_net[n].from, g_net[n].data);
    g_nnet = 0;
}

static void step(int rounds) {
    for (int r = 0; r < rounds; r++) {
        cable(&A);
        cable(&B);
        mp_online_tick(&A.o);
        mp_online_tick(&B.o);
        server();
    }
}

int main(void) {
    board_init(&A, 3);
    board_init(&B, 1);
    for (int i = 0; i < 3; i++) A.gba[i].slot = i;
    A.gba[0].parent = true;
    B.gba[0].slot = 3;

    /* roles, as the server gives them: A ready first */
    A.id = 0;
    B.id = 3;
    mp_online_command(&A.o, 4, MPO_CMD_SET_SLAVE, 0);
    mp_online_command(&B.o, 4, MPO_CMD_SET_MASTER, 3);
    check("A: its GBA on port 1 is the parent: slots 0-2 local", A.layout.first == 0 && A.layout.locals == 3, 1);
    check("B: stands in for the parent, its GBA slot 3", B.layout.first == 3 && B.layout.locals == 1, 1);
    check("... both buses started", A.bus_on && B.bus_on, 1);
    check("B's relay: only slot 3 is local", B.link->relay.local_mask, 0x8);
    mp_online_command(&B.o, 4, MPO_CMD_SET_MASTER, 3);
    check("a role given again keeps the link", B.bus_on && B.layout.first == 3, 1);

    step(4);
    check("every board's GBAs handshaking: StartHandshake to both", g_started, 1);
    step(40);
    check("A's parent GBA sent 8FFF: A's cable connected", A.link->relay.connected, 1);
    check("ConnectLink reached B: B sent 8FFF in slot 0", B.link->relay.connected, 1);
    check("every GBA in frames", A.gba[0].phase == 2 && A.gba[2].phase == 2 && B.gba[0].phase == 2, 1);

    /* commands: port 2's GBA on A (slot 1) and B's GBA (slot 3) */
    const uint16_t c1[8] = { 0xBBBB, 0x003C, 0x0081, 1, 2, 3, 4, 5 };
    const uint16_t c3[8] = { 0xBBBB, 0x003C, 0x0083, 9, 8, 7, 6, 5 };
    memcpy(A.gba[1].cmd, c1, sizeof c1); A.gba[1].have_cmd = true;
    memcpy(B.gba[0].cmd, c3, sizeof c3); B.gba[0].have_cmd = true;
    step(60);
    check("slot 1's command reached B's GBA", memcmp(B.gba[0].got[1], c1, sizeof c1), 0);
    check("slot 3's command reached all three of A's", !memcmp(A.gba[0].got[3], c3, sizeof c3) &&
          !memcmp(A.gba[1].got[3], c3, sizeof c3) && !memcmp(A.gba[2].got[3], c3, sizeof c3), 1);
    check("A's GBAs heard each other's on their own cable", memcmp(A.gba[2].got[1], c1, sizeof c1), 0);
    int errs = 0;
    for (int i = 0; i < 3; i++) errs += A.gba[i].checksum_errors;
    errs += B.gba[0].checksum_errors;
    check("no GBA saw a checksum it disagreed with", errs, 0);
    check("nothing refused", A.o.bad_packets + B.o.bad_packets, 0);
    check("A sent one packet, B one", A.o.packets_out == 1 && B.o.packets_out == 1, 1);

    /* what must not reach a GBA */
    uint8_t bad[64] = { 'M', 1, 1 };            /* a command for A's own slot 1 */
    mp_online_data(&A.o, 3, bad);
    check("a command for one of our own slots is refused", A.o.bad_packets, 1);
    memset(bad, 0, sizeof bad); bad[0] = 'M'; bad[1] = 4;
    mp_online_data(&A.o, 3, bad);
    check("... more entries than a packet holds", A.o.bad_packets, 2);
    memset(bad, 0, sizeof bad); bad[0] = 'X'; bad[1] = 1; bad[2] = 3;
    mp_online_data(&A.o, 3, bad);
    check("... the wrong magic", A.o.bad_packets, 3);
    memset(bad, 0, sizeof bad); bad[0] = 'M'; bad[1] = 1; bad[2] = 3; bad[63] = 1;
    mp_online_data(&A.o, 3, bad);
    check("... junk after the entries", A.o.bad_packets, 4);
    memset(bad, 0, sizeof bad); bad[0] = 'M'; bad[1] = 1; bad[2] = 4;
    mp_online_data(&A.o, 3, bad);
    check("... a slot past the link", A.o.bad_packets, 5);

    mp_online_command(&A.o, 4, MPO_CMD_CANCEL, -1);
    check("Cancel stops the bus", A.bus_on, 0);

    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
