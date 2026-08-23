/*
 * test_bridge.c -- two bridges wired back to back.
 *
 *   cc -std=c11 -Wall -Wextra -o test_bridge test_bridge.c \
 *      link_bridge.c link_proto.c link_session.c
 *
 * The loopback is deliberately hostile: it can fragment a stream at arbitrary
 * boundaries, drop datagrams, and corrupt bytes. Those are the three things a
 * real transport does -- USB fragments, ESP-NOW drops, and radios corrupt --
 * and a bridge that only works on a clean wire is not finished.
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "link_bridge.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-44s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* ---- loopback ---------------------------------------------------------- */

typedef struct wire {
    link_bridge_t *far;
    uint32_t      *clock;
    size_t         fragment;   /* 0 = whole, else bytes per delivery */
    bool           drop_next;
    bool           corrupt_next;
    uint32_t       delivered;
    uint32_t       dropped;
} wire_t;

static bool wire_tx(void *ctx, const uint8_t *data, size_t len) {
    wire_t *w = ctx;

    if (w->drop_next) {
        w->drop_next = false;
        w->dropped++;
        return true; /* the transport accepted it; the air did not */
    }

    uint8_t buf[LP_MAX_FRAMED];
    memcpy(buf, data, len);
    if (w->corrupt_next) {
        w->corrupt_next = false;
        buf[len / 2] ^= 0x40;
    }

    w->delivered++;
    if (w->fragment == 0) {
        bridge_feed(w->far, buf, len, *w->clock);
    } else {
        for (size_t off = 0; off < len; off += w->fragment) {
            size_t n = len - off;
            if (n > w->fragment) n = w->fragment;
            bridge_feed(w->far, buf + off, n, *w->clock);
        }
    }
    return true;
}

/* Bridges reference each other, so the contexts are set up after init. */
typedef struct {
    link_bridge_t a, b;
    wire_t        wa, wb;
    uint32_t      clock;
    gba_transfer_t last_result;
    uint16_t      last_seq;
    uint32_t      results;
} pair_t;

static pair_t g_pair;

static void pair_result_a(void *ctx, const gba_transfer_t *x, uint16_t seq) {
    (void)ctx;
    g_pair.last_result = *x;
    g_pair.last_seq = seq;
    g_pair.results++;
}

static void pair_init(pair_t *p, bool stream, size_t fragment) {
    memset(p, 0, sizeof *p);
    p->clock = 1000000;

    bridge_init(&p->a, wire_tx, &p->wa, stream, GBA_ROLE_MASTER);
    bridge_init(&p->b, wire_tx, &p->wb, stream, GBA_ROLE_SLAVE);

    p->wa.far = &p->b; p->wa.clock = &p->clock; p->wa.fragment = fragment;
    p->wb.far = &p->a; p->wb.clock = &p->clock; p->wb.fragment = fragment;

    p->a.ctx = &p->wa;
    p->b.ctx = &p->wb;
    p->a.on_result = pair_result_a;
}

static void connect(pair_t *p) {
    bridge_hello(&p->a, p->clock);
}

/* ---- tests -------------------------------------------------------------- */

static void test_handshake(void) {
    printf("handshake\n");

    pair_init(&g_pair, true, 0);
    pair_t *p = &g_pair;

    check("both start down", p->a.state, BRIDGE_DOWN);
    check("peer too", p->b.state, BRIDGE_DOWN);
    check("not ready", bridge_ready(&p->a), 0);

    connect(p);
    check("initiator ready", p->a.state, BRIDGE_READY);
    check("responder ready", p->b.state, BRIDGE_READY);

    /* A HELLO is answered with an ACK, not another HELLO -- otherwise two
     * devices powering on together would trade HELLOs forever. */
    check("one hello each way", p->a.hellos_sent, 1);
    check("responder did not re-hello", p->b.hellos_sent, 0);

    /* Both sides agree on the starting sequence. */
    check("sequences agree", p->a.session.seq, p->b.session.seq);
}

static void test_words_and_results(void) {
    printf("words and results\n");

    pair_init(&g_pair, true, 0);
    pair_t *p = &g_pair;
    connect(p);

    /* B is the far player; it pushes its words to A, which is the parent. */
    for (uint16_t s = 0; s < LS_DEPTH; s++) {
        check("word accepted", bridge_send_word(&p->b, s, (uint16_t)(0x200 + s)), 1);
    }
    check("all received", p->a.words_received, LS_DEPTH);
    check("session buffered them", ls_buffered(&p->a.session), LS_DEPTH);

    bool under = false;
    check("first word is right", ls_take_word(&p->a.session, 0, &under), 0x200);
    check("not an underrun", under, 0);
    ls_complete(&p->a.session);

    /* A reports the completed transfer back. */
    gba_transfer_t x;
    memset(&x, 0, sizeof x);
    x.word[0] = 0x1234; x.word[1] = 0x200;
    x.word[2] = GBA_WORD_EMPTY; x.word[3] = GBA_WORD_EMPTY;
    x.players = 2; x.self_id = 0;

    check("report sent", bridge_report(&p->a, &x, 0, false), 1);
    check("result delivered", p->b.results_received, 1);

    /* Words are refused before the link is up, rather than vanishing. */
    pair_t q;
    pair_init(&q, true, 0);
    check("no words while down", bridge_send_word(&q.a, 0, 0x1111), 0);
    check("no report while down", bridge_report(&q.a, &x, 0, false), 0);
}

static void test_fragmentation(void) {
    printf("stream fragmentation\n");

    /* One byte at a time is the worst case a USB stack can hand us. */
    for (size_t frag = 1; frag <= 5; frag++) {
        pair_init(&g_pair, true, frag);
        pair_t *p = &g_pair;
        connect(p);
        if (p->a.state != BRIDGE_READY || p->b.state != BRIDGE_READY) {
            printf("  FAIL handshake failed at fragment size %zu\n", frag);
            g_fail++;
            continue;
        }
        bridge_send_word(&p->b, 0, 0xABCD);
        if (p->a.words_received != 1) {
            printf("  FAIL word lost at fragment size %zu\n", frag);
            g_fail++;
        }
    }
    check("survives any fragmentation", g_fail, 0);
}

static void test_corruption(void) {
    printf("corruption and loss\n");

    pair_init(&g_pair, true, 0);
    pair_t *p = &g_pair;
    connect(p);

    /* A corrupted frame must be dropped, not misinterpreted. */
    p->wb.corrupt_next = true;
    bridge_send_word(&p->b, 0, 0x1111);
    check("corrupt frame rejected", p->a.words_received, 0);
    check("counted as bad", p->a.bad_packets > 0, 1);

    /* And the next clean one still lands: the receiver resynchronises. */
    bridge_send_word(&p->b, 0, 0x2222);
    check("recovers", p->a.words_received, 1);

    bool under = false;
    check("the good word arrived", ls_take_word(&p->a.session, 0, &under), 0x2222);

    /* A dropped datagram is simply absent; the slot underruns, which is a
     * state the hardware already produces. */
    pair_init(&g_pair, false, 0);
    connect(p);
    p->wb.drop_next = true;
    bridge_send_word(&p->b, p->a.session.seq, 0x3333);
    check("dropped word never arrives", p->a.words_received, 0);
    check("session reports not ready", ls_ready(&p->a.session), 0);
    check("underruns cleanly",
          ls_take_word(&p->a.session, 8000, &under), GBA_WORD_EMPTY);
    check("flagged", under, 1);
}

static void test_datagrams(void) {
    printf("datagram transport\n");

    pair_init(&g_pair, false, 0);
    pair_t *p = &g_pair;
    connect(p);
    check("handshake over datagrams", p->a.state, BRIDGE_READY);

    bridge_send_word(&p->b, 0, 0x55AA);
    check("word arrived", p->a.words_received, 1);

    /* Garbage on the air must not be mistaken for a packet. */
    uint8_t junk[12];
    memset(junk, 0xA5, sizeof junk);
    uint32_t before = p->a.bad_packets;
    bridge_feed(&p->a, junk, sizeof junk, p->clock);
    check("junk rejected", p->a.bad_packets, (long)before + 1);
    check("no word invented", p->a.words_received, 1);
}

static void test_ping(void) {
    printf("round trip measurement\n");

    pair_init(&g_pair, true, 0);
    pair_t *p = &g_pair;
    connect(p);

    uint32_t sent_at = p->clock;
    bridge_ping(&p->a, sent_at);
    /* The pong echoes the original stamp rather than restamping, so the
     * measurement is a true round trip and needs no clock agreement. */
    p->clock = sent_at + 1500;
    bridge_ping(&p->a, p->clock - 1500);
    check("rtt measured", p->a.rtt_us > 0, 1);

    /* A wrapped microsecond counter must not produce an absurd figure. */
    p->a.rtt_us = 0;
    p->clock = 10;
    lp_packet_t pong;
    memset(&pong, 0, sizeof pong);
    pong.type = LP_PONG;
    pong.u.ping.stamp_us = 0xFFFFFF00u;
    uint8_t buf[LP_MAX_FRAMED];
    size_t n = lp_encode(&pong, buf, sizeof buf);
    bridge_feed(&p->a, buf, n, 10);
    check("survives a clock wrap", p->a.rtt_us, 10 - 0xFFFFFF00u + 0);
}

static void test_recovery(void) {
    printf("peer disappearing and returning\n");

    pair_init(&g_pair, true, 0);
    pair_t *p = &g_pair;
    connect(p);
    check("ready", p->a.state, BRIDGE_READY);

    /* Silence past the timeout must drop the session rather than leaving
     * stale words looking current. */
    p->clock += BRIDGE_TIMEOUT_US + 1;
    bridge_tick(&p->a, p->clock);
    check("declared down", p->a.state, BRIDGE_DOWN);
    check("words refused again", bridge_send_word(&p->a, 0, 0x1234), 0);

    /* tick re-announces, and the peer answers. */
    p->clock += BRIDGE_HELLO_INTERVAL_US;
    bridge_tick(&p->a, p->clock);
    check("re-announced", p->a.hellos_sent, 2);
    check("back up", p->a.state, BRIDGE_READY);

    /* An explicit reset resynchronises both sequence counters. */
    bridge_reset(&p->a, 500);
    check("local sequence moved", p->a.session.seq, 500);
    check("peer followed", p->b.session.seq, 500);
}

static void test_version_mismatch(void) {
    printf("version mismatch\n");

    pair_init(&g_pair, true, 0);
    pair_t *p = &g_pair;

    /* A peer speaking a different version is worse than no peer, because it
     * would half-work. Refuse and keep announcing. */
    lp_packet_t ack;
    memset(&ack, 0, sizeof ack);
    ack.type = LP_HELLO_ACK;
    ack.seq  = 0;
    ack.u.hello.version = LP_VERSION + 1;
    ack.u.hello.role = GBA_ROLE_SLAVE;
    ack.u.hello.players = 2;

    uint8_t buf[LP_MAX_FRAMED];
    size_t n = lp_encode(&ack, buf, sizeof buf);
    bridge_hello(&p->a, p->clock);
    p->a.state = BRIDGE_HELLO; /* pretend the real peer never answered */
    bridge_feed(&p->a, buf, n, p->clock);

    check("not accepted", p->a.state, BRIDGE_HELLO);
    check("counted as ignored", p->a.ignored, 1);
}

int main(void) {
    test_handshake();
    test_words_and_results();
    test_fragmentation();
    test_corruption();
    test_datagrams();
    test_ping();
    test_recovery();
    test_version_mismatch();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
