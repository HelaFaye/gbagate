/*
 * test_mprelay.c -- src/mp_relay.c, the four-player relay's logic.
 *
 * test/mgba/link4.py runs it under four real Emeralds (with the user's ROM);
 * this checks each rule on its own, from scripted cable words, so CI does.
 */
#include <stdio.h>
#include <string.h>

#include "mp_relay.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-58s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* One transfer: the relay's words, the local GBA's in its slot, then done. */
static void xfer(mp_relay_t *r, uint16_t local_word, uint16_t got[MP_MAX_PLAYERS]) {
    uint16_t w[MP_MAX_PLAYERS] = {0};
    mp_tx(r, w);
    w[r->local] = local_word;
    if (got) memcpy(got, w, sizeof w);
    mp_done(r, w);
}

static void test_handshake_parent(void) {
    mp_relay_t r;
    uint16_t w[4];
    mp_init(&r, 4, 1u << 0);                       /* the parent's cable */
    xfer(&r, MP_SLAVE_HANDSHAKE, w);
    check("before StartHandshake, nobody in the remote slots", w[1] == 0xFFFF && w[2] == 0xFFFF && w[3] == 0xFFFF, 1);
    check("... and the GBA's handshake is seen", r.local_ready, 1);
    mp_start_handshake(&r);
    xfer(&r, MP_SLAVE_HANDSHAKE, w);
    check("after, B9A0 for every remote player", w[1] == MP_SLAVE_HANDSHAKE && w[2] == MP_SLAVE_HANDSHAKE && w[3] == MP_SLAVE_HANDSHAKE, 1);
    mp_connect(&r);
    check("ConnectLink means nothing on the parent's own cable", r.connect, 0);
    xfer(&r, MP_MASTER_HANDSHAKE, w);        /* the parent GBA connects */
    check("the parent's 8FFF connects", r.connected && r.phase == MP_CHECKSUM_SLOT, 1);
    xfer(&r, MP_SLAVE_HANDSHAKE, w);
    check("the unchecked checksum slot: B9A0", w[1], MP_SLAVE_HANDSHAKE);
    check("... then frames", r.phase, MP_FRAMES);
}

static void test_handshake_child(void) {
    mp_relay_t r;
    uint16_t w[4];
    mp_init(&r, 4, 1u << 2);                       /* child 2's cable: slot 0 is ours */
    mp_start_handshake(&r);
    xfer(&r, MP_SLAVE_HANDSHAKE, w);
    check("a child's cable: slot 0 handshakes as the parent", w[0], MP_SLAVE_HANDSHAKE);
    check("... the GBA's own slot is its own", w[2], MP_SLAVE_HANDSHAKE);
    mp_connect(&r);
    xfer(&r, MP_SLAVE_HANDSHAKE, w);
    check("8FFF waits for two full rounds", w[0], MP_SLAVE_HANDSHAKE);
    xfer(&r, MP_SLAVE_HANDSHAKE, w);
    check("then slot 0 sends 8FFF", w[0], MP_MASTER_HANDSHAKE);
    check("... and the cable is connected", r.connected, 1);

    mp_relay_t q;
    mp_init(&q, 4, 1u << 1);
    mp_start_handshake(&q);
    mp_connect(&q);
    xfer(&q, MP_SLAVE_HANDSHAKE, w);
    xfer(&q, 0x1234, w);                     /* the GBA left the handshake */
    xfer(&q, MP_SLAVE_HANDSHAKE, w);
    check("a broken round starts the count again", w[0], MP_SLAVE_HANDSHAKE);
}

/* Bring a relay to its first frame. */
static void to_frames(mp_relay_t *r) {
    mp_start_handshake(r);
    if (r->local) {
        mp_connect(r);
        for (int i = 0; i < 4 && r->phase == MP_HANDSHAKE; i++) xfer(r, MP_SLAVE_HANDSHAKE, NULL);
    } else {
        xfer(r, MP_MASTER_HANDSHAKE, NULL);
    }
    xfer(r, MP_SLAVE_HANDSHAKE, NULL);       /* the checksum slot */
}

static void test_frames(void) {
    mp_relay_t r;
    uint16_t w[4], cmd[8];
    mp_init(&r, 4, 1u << 0);
    const uint16_t a[8] = { 0xBBBB, 0x003C, 0x0081, 0, 0, 0, 0, 0 };
    const uint16_t b[8] = { 0xBBBB, 0x003C, 0x0082, 0, 0, 0, 0, 0 };
    check("push from player 1", mp_push(&r, 1, a), 1);
    check("push from player 2", mp_push(&r, 2, b), 1);
    const uint16_t c[8] = { 0xBBBB, 0x003C, 0x0083, 0, 0, 0, 0, 0 };
    check("push from player 3", mp_push(&r, 3, c), 1);
    check("a push from the local player is refused", mp_push(&r, 0, a), 0);
    check("... and from a slot past the link", mp_push(&r, 4, a), 0);
    to_frames(&r);

    const uint16_t mine[8] = { 0xBBBB, 0x003C, 0x0080, 0, 0, 0, 0, 0 };
    uint32_t sum = 0;
    for (int k = 0; k < 8; k++) {
        xfer(&r, mine[k], w);
        check("player 1's command in slot 1, word by word", w[1], a[k]);
        check("player 2's in slot 2", w[2], b[k]);
        check("player 3's in slot 3", w[3], c[k]);
        sum += w[0] + w[1] + w[2] + w[3];
    }
    xfer(&r, (uint16_t)sum, w);
    check("the checksum word, in every slot it answers, is the frame's sum",
          w[1] == (uint16_t)sum && w[2] == (uint16_t)sum && w[3] == (uint16_t)sum, 1);
    check("(the trace's frame 4 sums to F1E2)", (uint16_t)sum, 0xF1E2);
    check("the GBA's own checksum agreed", r.bad_checksums, 0);
    check("the GBA's command goes out", mp_pop(&r, NULL, cmd) && memcmp(cmd, mine, sizeof cmd) == 0, 1);
    check("one frame done", r.frames, 1);

    for (int k = 0; k < 8; k++) xfer(&r, 0, w);
    xfer(&r, 0, w);
    check("an idle frame is not relayed", mp_pop(&r, NULL, cmd), 0);
    check("... and with the queues empty the remote slots idle too", w[1] == 0 && w[2] == 0 && w[3] == 0, 1);

    for (int k = 0; k < 8; k++) xfer(&r, k == 3 ? 7 : 0, w);
    xfer(&r, 1, w);                          /* not the frame's sum (7) */
    check("a GBA checksum that disagrees is counted", r.bad_checksums, 1);
}

static void test_queue(void) {
    mp_relay_t r;
    uint16_t c[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    mp_init(&r, 4, 1u << 3);
    int ok = 0;
    for (int i = 0; i < MP_QUEUE + 3; i++) ok += mp_push(&r, 0, c);
    check("a remote player's queue holds MP_QUEUE commands", ok, MP_QUEUE);
    check("... the rest dropped, counted", r.dropped, 3);
    mp_init(&r, 2, 1u << 1);
    check("a two-player link: slot 2 is nobody's", mp_push(&r, 2, c), 0);
    uint16_t w[4];
    xfer(&r, MP_SLAVE_HANDSHAKE, w);
    check("... and answers FFFF", w[2] == 0xFFFF && w[3] == 0xFFFF, 1);
}

/* Three GBAs on the board's cable (slots 0-2), one remote player (slot 3). */
static void test_three_local(void) {
    mp_relay_t r;
    uint16_t w[4], cmd[8];
    uint8_t from;
    mp_init(&r, 4, 0x7);
    w[0] = w[1] = w[2] = MP_SLAVE_HANDSHAKE;
    mp_tx(&r, w);
    check("before the session the board answers nothing", w[3], 0xFFFF);
    mp_done(&r, w);
    check("three GBAs detected on the cable", mp_detect_count(&r), 3);
    check("... in slots 0-2", mp_detect(&r), 0x7);
    check("... all of them handshaking", r.local_ready, 1);
    mp_connect(&r);
    check("a local GBA is the parent: ConnectLink is not the board's to act on", r.connect, 0);

    mp_start_handshake(&r);
    w[0] = MP_MASTER_HANDSHAKE;
    mp_tx(&r, w);
    check("after the start the board answers slot 3 only", w[3], MP_SLAVE_HANDSHAKE);
    mp_done(&r, w);
    check("the local parent's 8FFF connects", r.connected, 1);
    w[0] = w[1] = w[2] = MP_SLAVE_HANDSHAKE;
    mp_tx(&r, w);
    mp_done(&r, w);                            /* the checksum slot */
    const uint16_t remote3[8] = { 0x2222, 0, 0, 0, 0, 0, 0, 0 };
    mp_push(&r, 3, remote3);
    check("a push for a local slot is refused", mp_push(&r, 1, remote3), 0);
    /* frames start at the checksum slot; this one began with the queue
     * empty, so the remote command comes the frame after */
    uint32_t sum = 0;
    for (int k = 0; k < 8; k++) {
        w[0] = (uint16_t)(k == 0 ? 0x1111 : 0); w[1] = (uint16_t)(k == 0 ? 0x3333 : 0); w[2] = 0;
        mp_tx(&r, w);
        sum += w[0] + w[1] + w[2] + w[3];
        mp_done(&r, w);
    }
    w[0] = w[1] = w[2] = (uint16_t)sum;
    mp_tx(&r, w);
    check("the board's checksum in slot 3 is the frame's", w[3], (uint16_t)sum);
    mp_done(&r, w);
    check("no bad checksums from any local GBA", r.bad_checksums, 0);
    check("slot 0's command goes out, marked 0", mp_pop(&r, &from, cmd) && from == 0 && cmd[0] == 0x1111, 1);
    check("slot 1's, marked 1", mp_pop(&r, &from, cmd) && from == 1 && cmd[0] == 0x3333, 1);
    check("slot 2 was idle: nothing", mp_pop(&r, &from, cmd), 0);
    mp_tx(&r, w);
    check("the remote player's command in slot 3, next frame", w[3], 0x2222);
}

int main(void) {
    test_three_local();
    test_handshake_parent();
    test_handshake_child();
    test_frames();
    test_queue();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
