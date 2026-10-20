/*
 * test_peer.c -- the wireless link cable's engine (gbl_peer.c) on its own.
 *
 * Two engines, a radio between them that loses, duplicates and reorders on a
 * fixed pseudo-random schedule, and for each a scripted stand-in for the
 * GB-Link firmware that records every command injected and answers with the
 * status words the real firmware sends. test/gblink_sim/test_peer.py runs the
 * same engine against the real firmware; this one runs at -O3 and under ASan.
 */
#include <stdio.h>
#include <string.h>

#include "gbl_peer.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-50s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* ---- a radio with a schedule ---------------------------------------------------- */

#define AIR_SLOTS 256
typedef struct {
    uint8_t  d[GBL_PEER_MAX_DATAGRAM];
    size_t   n;
    int      to;
    uint32_t at;
} flight_t;

static flight_t s_air[AIR_SLOTS];
static uint32_t s_rng = 12345, s_now;
static int      s_loss_pct, s_dup_pct, s_jitter;
static uint32_t s_dropped;

static uint32_t rnd(void) { s_rng = s_rng * 1103515245u + 12345u; return s_rng >> 8; }

static void air_put(int to, const uint8_t *d, size_t n) {
    for (int copies = 1 + ((int)(rnd() % 100) < s_dup_pct); copies > 0; copies--) {
        if ((int)(rnd() % 100) < s_loss_pct) { s_dropped++; continue; }
        for (int i = 0; i < AIR_SLOTS; i++) {
            if (s_air[i].n == 0) {
                memcpy(s_air[i].d, d, n);
                s_air[i].n = n;
                s_air[i].to = to;
                s_air[i].at = s_now + 1 + (s_jitter ? rnd() % (uint32_t)s_jitter : 0);
                break;
            }
        }
    }
}

/* ---- a firmware stand-in ------------------------------------------------------------ */

typedef struct {
    int      id;
    gbl_peer_t peer;
    uint8_t  log[256];      /* injected command opcodes, in order */
    int      nlog;
    uint16_t data_in[64];   /* first word of each injected data packet */
    int      ndata;
    bool     mode_set, is_master, hs, up;
} board_t;

static board_t s_b[2];

static void inject_cmd(void *ctx, const uint8_t *c, size_t n) {
    board_t *b = ctx;
    (void)n;
    if (b->nlog < 256) b->log[b->nlog++] = c[0];
    switch (c[0]) {
    case 0x00: b->mode_set = true; gbl_peer_on_status(&b->peer, 0xFF08, s_now);
               gbl_peer_on_status(&b->peer, 0xFF02, s_now); break;
    case 0x10: b->is_master = true;  gbl_peer_on_status(&b->peer, 0xFF03, s_now); break;
    case 0x11: b->is_master = false; gbl_peer_on_status(&b->peer, 0xFF03, s_now); break;
    case 0x12: b->hs = true;
               /* The slave's GBA is the parent and connects by itself. */
               if (!b->is_master) { b->up = true; gbl_peer_on_status(&b->peer, 0xFF05, s_now); }
               break;
    case 0x13: if (b->is_master && b->hs && !b->up) { b->up = true; gbl_peer_on_status(&b->peer, 0xFF05, s_now); }
               break;
    default: break;
    }
}

static void inject_data(void *ctx, const uint8_t *d, size_t n) {
    board_t *b = ctx;
    if (n == 64 && b->ndata < 64) b->data_in[b->ndata++] = (uint16_t)(d[0] | (d[1] << 8));
}

static bool radio_tx(void *ctx, const uint8_t *d, size_t n) {
    board_t *b = ctx;
    air_put(1 - b->id, d, n);
    return true;
}

static uint8_t s_mode[2] = { 0x01, 0x01 };

static void setup(int loss, int dup, int jitter) {
    memset(s_air, 0, sizeof s_air);
    memset(s_b, 0, sizeof s_b);
    s_now = 1000;
    s_loss_pct = loss; s_dup_pct = dup; s_jitter = jitter; s_dropped = 0;
    for (int i = 0; i < 2; i++) {
        s_b[i].id = i;
        gbl_peer_io_t io = { inject_cmd, inject_data, radio_tx, &s_b[i] };
        gbl_peer_init(&s_b[i].peer, &io, i == 0 ? 0xC0FFEEu : 0x0BEEFu, s_mode[i], 0);
        gbl_peer_start(&s_b[i].peer, s_now);
    }
}

static void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t++) {
        s_now++;
        for (int i = 0; i < AIR_SLOTS; i++) {
            if (s_air[i].n && s_air[i].at <= s_now) {
                size_t n = s_air[i].n;
                s_air[i].n = 0;
                gbl_peer_on_radio(&s_b[s_air[i].to].peer, s_air[i].d, n, s_now);
            }
        }
        for (int i = 0; i < 2; i++) gbl_peer_tick(&s_b[i].peer, s_now);
    }
}

static int find(const board_t *b, uint8_t op) {
    for (int i = 0; i < b->nlog; i++) if (b->log[i] == op) return i;
    return -1;
}

static void test_session(const char *name, int loss, int dup, int jitter) {
    printf("%s\n", name);
    setup(loss, dup, jitter);
    run(3000);
    check("board 0 is master (larger nonce)", s_b[0].peer.master, 1);
    check("board 1 is slave", s_b[1].peer.master, 0);
    for (int i = 0; i < 2; i++) {
        board_t *b = &s_b[i];
        check("Cancel before SetMode", find(b, 0x01) >= 0 && find(b, 0x01) < find(b, 0x00), 1);
        check("role command after SetMode", find(b, i == 0 ? 0x10 : 0x11) > find(b, 0x00), 1);
        check("StartHandshake once both handshakes seen", find(b, 0x12) > 0, 1);
        check("linked", b->peer.state, GBL_PEER_LINKED);
    }
    check("master got ConnectLink after the slave connected", find(&s_b[0], 0x13) > find(&s_b[0], 0x12), 1);

    /* Data both ways, in order, exactly once, whatever the air does. */
    for (uint16_t k = 0; k < 40; k++) {
        uint8_t pkt[64] = { 0 };
        pkt[0] = (uint8_t)k; pkt[1] = (uint8_t)(k >> 8);
        gbl_peer_on_data(&s_b[0].peer, pkt, 64, s_now);
        pkt[1] = 0x80;
        gbl_peer_on_data(&s_b[1].peer, pkt, 64, s_now);
        run(3);
    }
    run(2000);
    check("all 40 packets 0->1", s_b[1].ndata, 40);
    check("all 40 packets 1->0", s_b[0].ndata, 40);
    int order = 1;
    for (int k = 0; k < s_b[1].ndata; k++) if (s_b[1].data_in[k] != k) order = 0;
    for (int k = 0; k < s_b[0].ndata; k++) if (s_b[0].data_in[k] != (0x8000 | k)) order = 0;
    check("in order", order, 1);
    if (loss) check("the air did lose datagrams", s_dropped > 0, 1);
}

static void test_garbage(void) {
    printf("garbage and strangers\n");
    setup(0, 0, 0);
    uint8_t junk[GBL_PEER_MAX_DATAGRAM];
    for (size_t n = 0; n <= sizeof junk; n++) {
        for (size_t i = 0; i < n; i++) junk[i] = (uint8_t)rnd();
        if (n > 2) { junk[0] = 'G'; junk[1] = 'P'; }
        gbl_peer_on_radio(&s_b[0].peer, junk, n, s_now);
    }
    check("every malformed datagram rejected", s_b[0].peer.bad_datagrams, (long)sizeof junk + 1);
    run(3000);
    check("and the real session still comes up", s_b[0].peer.state, GBL_PEER_LINKED);
}

static void test_timeout(void) {
    printf("peer gone\n");
    setup(0, 0, 0);
    run(3000);
    int cancels_before = 0;
    for (int i = 0; i < s_b[0].nlog; i++) cancels_before += s_b[0].log[i] == 0x01;
    s_loss_pct = 100;
    run(GBL_PEER_TIMEOUT_MS + 100);
    int cancels = 0;
    for (int i = 0; i < s_b[0].nlog; i++) cancels += s_b[0].log[i] == 0x01;
    check("local session cancelled when the peer went silent", cancels, cancels_before + 1);
    check("searching again", s_b[0].peer.state, GBL_PEER_SEARCHING);
}

static void test_mode_mismatch(void) {
    printf("boards in different modes\n");
    s_mode[0] = 0x01; s_mode[1] = 0x07;   /* a Gen 3 cable and a Wireless Adapter */
    setup(0, 0, 0);
    run(3000);
    check("neither takes the other for a peer", s_b[0].peer.state == GBL_PEER_SEARCHING &&
                                                s_b[1].peer.state == GBL_PEER_SEARCHING, 1);
    check("and says why", s_b[0].peer.mode_mismatch > 0 && s_b[1].peer.mode_mismatch > 0, 1);
    check("no mode was set on either", find(&s_b[0], 0x00) < 0 && find(&s_b[1], 0x00) < 0, 1);
    s_mode[1] = 0x01;
}

static void test_rfu_telemetry(void) {
    printf("Wireless Adapter telemetry\n");
    s_mode[0] = s_mode[1] = 0x07;
    setup(0, 0, 0);
    gbl_peer_t *p = &s_b[0].peer;
    check("nothing seen yet", p->rfu.seen, 0);
    uint8_t det[16] = { 0x0E, 1, 1, 3 };          /* any rx, NINTENDO, comstate 3 */
    det[12] = 0x34; det[13] = 0x12;
    gbl_peer_on_data(p, det, sizeof det, s_now);
    check("detection noted", p->rfu.seen && p->rfu.any_rx && p->rfu.found, 1);
    check("transfers", (long)p->rfu.transfers, 0x1234);
    uint8_t ev[25] = { 0x1D };
    ev[14] = 7;
    gbl_peer_on_data(p, ev, sizeof ev, s_now);
    check("soft resets", p->rfu.resets, 7);
    det[3] = 0;                                    /* back in the ID wait */
    gbl_peer_on_data(p, det, sizeof det, s_now);
    check("found stays found", p->rfu.found, 1);
    check("telemetry never crosses the cable", (long)p->data_out, 0);
    s_mode[0] = s_mode[1] = 0x01;
}

int main(void) {
    test_rfu_telemetry();
    test_mode_mismatch();
    test_session("clean air", 0, 0, 0);
    test_session("lossy air (25% loss, 10% duplicates, reordering)", 25, 10, 30);
    test_garbage();
    test_timeout();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
