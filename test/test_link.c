/*
 * test_link.c -- host-side tests for the bridge protocol and the pacing logic.
 *
 *   cc -std=c11 -Wall -Wextra -o test_link test_link.c link_proto.c link_session.c
 */

#include <stdio.h>
#include <string.h>

#include "link_proto.h"
#include "link_session.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-40s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* ---- protocol -------------------------------------------------------- */

static bool roundtrip(const lp_packet_t *in, lp_packet_t *out) {
    uint8_t framed[LP_MAX_FRAMED];
    size_t n = lp_encode(in, framed, sizeof framed);
    if (n == 0) {
        return false;
    }
    lp_rx_t rx;
    lp_rx_init(&rx);
    bool got = false;
    for (size_t i = 0; i < n; i++) {
        if (lp_rx_feed(&rx, framed[i], out)) {
            got = true;
        }
    }
    return got;
}

static void test_proto_roundtrip(void) {
    printf("protocol round trip\n");

    lp_packet_t in = { .type = LP_WORD, .seq = 0x1234 };
    in.u.word.word = 0xBEEF;
    lp_packet_t out;
    check("WORD decodes", roundtrip(&in, &out), 1);
    check("WORD type", out.type, LP_WORD);
    check("WORD seq", out.seq, 0x1234);
    check("WORD payload", out.u.word.word, 0xBEEF);

    memset(&in, 0, sizeof in);
    in.type = LP_RESULT;
    in.seq  = 7;
    in.u.result.word[0] = 0x0001;
    in.u.result.word[1] = 0x00C0;
    in.u.result.word[2] = GBA_WORD_EMPTY;
    in.u.result.word[3] = GBA_WORD_EMPTY;
    in.u.result.players = 2;
    in.u.result.self_id = 0;
    check("RESULT decodes", roundtrip(&in, &out), 1);
    check("RESULT w0", out.u.result.word[0], 0x0001);
    check("RESULT w1", out.u.result.word[1], 0x00C0);
    check("RESULT w2 empty", out.u.result.word[2], GBA_WORD_EMPTY);
    check("RESULT players", out.u.result.players, 2);

    memset(&in, 0, sizeof in);
    in.type = LP_HELLO;
    in.u.hello.version = LP_VERSION;
    in.u.hello.role    = GBA_ROLE_MASTER;
    in.u.hello.players = 2;
    check("HELLO decodes", roundtrip(&in, &out), 1);
    check("HELLO role", out.u.hello.role, GBA_ROLE_MASTER);
}

/* SLIP has to survive payloads containing its own delimiters. */
static void test_proto_escaping(void) {
    printf("SLIP escaping\n");

    lp_packet_t in = { .type = LP_WORD, .seq = 0xDBC0 };
    in.u.word.word = 0xC0DB; /* END and ESC in the payload */
    lp_packet_t out;
    check("escaped payload survives", roundtrip(&in, &out), 1);
    check("seq intact", out.seq, 0xDBC0);
    check("word intact", out.u.word.word, 0xC0DB);
}

static void test_proto_corruption(void) {
    printf("corruption rejection\n");

    lp_packet_t in = { .type = LP_WORD, .seq = 5 };
    in.u.word.word = 0x1111;
    uint8_t framed[LP_MAX_FRAMED];
    size_t n = lp_encode(&in, framed, sizeof framed);

    /* Flip a bit in the middle of the frame. */
    framed[3] ^= 0x01;

    lp_rx_t rx;
    lp_rx_init(&rx);
    lp_packet_t out;
    bool got = false;
    for (size_t i = 0; i < n; i++) {
        if (lp_rx_feed(&rx, framed[i], &out)) {
            got = true;
        }
    }
    check("bad CRC rejected", got, 0);
    check("drop counted", rx.dropped, 1);

    /* And the receiver must resynchronise on the next clean frame. */
    n = lp_encode(&in, framed, sizeof framed);
    got = false;
    for (size_t i = 0; i < n; i++) {
        if (lp_rx_feed(&rx, framed[i], &out)) {
            got = true;
        }
    }
    check("resyncs after garbage", got, 1);
    check("recovered word", out.u.word.word, 0x1111);
}

/* ---- pacing ---------------------------------------------------------- */

static void test_session_happy(void) {
    printf("pacing: peer keeps up\n");

    link_session_t ls;
    ls_init(&ls, LS_DEFAULT_STALL_US);

    check("nothing buffered", ls_buffered(&ls), 0);
    check("not ready", ls_ready(&ls), 0);

    for (uint16_t s = 0; s < LS_DEPTH; s++) {
        check("push accepted", ls_push_word(&ls, s, (uint16_t)(0x100 + s)), 1);
    }
    check("window full", ls_buffered(&ls), LS_DEPTH);

    for (uint16_t s = 0; s < LS_DEPTH; s++) {
        bool under = true;
        check("ready", ls_ready(&ls), 1);
        check("word", ls_take_word(&ls, 0, &under), 0x100 + s);
        check("no underrun", under, 0);
        ls_complete(&ls);
    }
    check("no stalls", ls.stalls, 0);
    check("no underruns", ls.underruns, 0);
    check("transfers", ls.transfers, LS_DEPTH);
}

static void test_session_underrun(void) {
    printf("pacing: peer is late\n");

    link_session_t ls;
    ls_init(&ls, 1000);

    bool under = false;
    check("not ready", ls_ready(&ls), 0);
    check("sends 0xFFFF", ls_take_word(&ls, 1000, &under), GBA_WORD_EMPTY);
    check("flagged underrun", under, 1);
    check("stall counted", ls.stalls, 1);
    check("peak wait recorded", ls.peak_wait_us, 1000);
    ls_complete(&ls);

    /* The late word now belongs to a transfer that already went out. */
    check("late word rejected", ls_push_word(&ls, 0, 0xAAAA), 0);
    check("late counted", ls.late, 1);

    /* But the peer catching up on the current sequence works fine. */
    check("current seq accepted", ls_push_word(&ls, 1, 0xBBBB), 1);
    check("ready again", ls_ready(&ls), 1);
    check("word", ls_take_word(&ls, 0, &under), 0xBBBB);
    check("recovered", under, 0);
}

static void test_session_window(void) {
    printf("pacing: window discipline\n");

    link_session_t ls;
    ls_init(&ls, LS_DEFAULT_STALL_US);

    check("beyond window rejected", ls_push_word(&ls, LS_DEPTH, 0x1234), 0);
    check("in window accepted", ls_push_word(&ls, LS_DEPTH - 1, 0x1234), 1);
    check("duplicate rejected", ls_push_word(&ls, LS_DEPTH - 1, 0x5678), 0);
    check("dupe counted", ls.dupes, 1);

    /* A gap in the middle must not be reported as buffered depth. */
    check("push seq 0", ls_push_word(&ls, 0, 0xAAAA), 1);
    check("contiguous run is 1", ls_buffered(&ls), 1);
}

/* Sequence numbers are 16 bit and this thing runs at 60 transfers a second,
 * so it wraps every ~18 minutes. It must not misbehave when it does. */
static void test_session_wrap(void) {
    printf("pacing: sequence wrap\n");

    link_session_t ls;
    ls_init(&ls, LS_DEFAULT_STALL_US);
    ls_reset(&ls, 0xFFFE);

    check("push 0xFFFE", ls_push_word(&ls, 0xFFFE, 0x1111), 1);
    check("push 0xFFFF", ls_push_word(&ls, 0xFFFF, 0x2222), 1);
    check("push 0x0000", ls_push_word(&ls, 0x0000, 0x3333), 1);
    check("push 0x0001", ls_push_word(&ls, 0x0001, 0x4444), 1);
    check("buffered across wrap", ls_buffered(&ls), 4);

    bool under;
    check("w 0xFFFE", ls_take_word(&ls, 0, &under), 0x1111); ls_complete(&ls);
    check("w 0xFFFF", ls_take_word(&ls, 0, &under), 0x2222); ls_complete(&ls);
    check("w 0x0000", ls_take_word(&ls, 0, &under), 0x3333); ls_complete(&ls);
    check("w 0x0001", ls_take_word(&ls, 0, &under), 0x4444); ls_complete(&ls);
    check("seq wrapped", ls.seq, 0x0002);
}

/* ---- a readable end-to-end trace ------------------------------------- */

static void trace(void) {
    printf("\ntrace: adapter as parent, peer jitters on transfer 2\n");

    link_session_t ls;
    ls_init(&ls, 8000);

    struct { uint16_t seq; uint16_t word; uint32_t wait_us; } script[] = {
        { 0, 0x0100,    0 },
        { 1, 0x0101,    0 },
        { 2, 0xFFFF, 8000 }, /* peer missed the budget */
        { 3, 0x0103,  900 }, /* arrived late but inside it */
        { 4, 0x0104,    0 },
    };

    for (unsigned i = 0; i < sizeof script / sizeof script[0]; i++) {
        if (script[i].word != 0xFFFF) {
            ls_push_word(&ls, script[i].seq, script[i].word);
        }
        bool under = false;
        uint16_t w = ls_take_word(&ls, script[i].wait_us, &under);
        printf("  seq %u  wait %5u us  bus=%04X  %s\n",
               script[i].seq, script[i].wait_us, w,
               under ? "UNDERRUN -> slot reads as absent" : "ok");
        ls_complete(&ls);
    }
    printf("  totals: %u transfers, %u stalls, %u underruns, peak wait %u us\n",
           (unsigned)ls.transfers, (unsigned)ls.stalls,
           (unsigned)ls.underruns, (unsigned)ls.peak_wait_us);
}

int main(void) {
    test_proto_roundtrip();
    test_proto_escaping();
    test_proto_corruption();
    test_session_happy();
    test_session_underrun();
    test_session_window();
    test_session_wrap();
    trace();

    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
