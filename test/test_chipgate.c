/*
 * test_chipgate.c -- host-side exercise of the gate state machine.
 *
 * Simulates the game side of the link so the responder can be validated with
 * no hardware and no emulator. Build:
 *
 *     cc -std=c11 -Wall -Wextra -o test_chipgate test_chipgate.c chipgate.c
 */

#include <stdio.h>
#include <string.h>

#include "chipgate.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-38s got %ld (0x%lX), want %ld (0x%lX)\n",
               what, got, (unsigned long)got, want, (unsigned long)want);
        g_fail++;
    }
}

/* Drive one frame as if we were the GBA. */
static uint16_t frame(chipgate_t *cg, uint16_t game_word) {
    return chipgate_step(cg, game_word);
}

/* Bring the link up the way a real game does. */
static void handshake(chipgate_t *cg, uint16_t ack) {
    frame(cg, CG_WORD_SYNC);
    frame(cg, ack);
}

static void test_handshake(void) {
    printf("handshake\n");
    chipgate_t cg;
    chipgate_init(&cg, CG_GAME_BN6_US);

    check("idle emits gate id", frame(&cg, 0x0000), CG_ID_BEAST_LINK_US);
    check("still idle", cg.state, CG_LINK_IDLE);

    check("ping answered with gate id", frame(&cg, CG_WORD_SYNC), CG_ID_BEAST_LINK_US);
    check("synced", cg.state, CG_LINK_SYNCED);

    frame(&cg, CG_ID_BEAST_LINK_US);
    check("linked", cg.state, CG_LINK_ACTIVE);
    check("session counted", cg.sessions, 1);
    check("resumes at UNK_1", cg.slot, CG_SLOT_UNK_1);
    check("connected()", chipgate_connected(&cg), 1);
}

static void test_rotation(void) {
    printf("nine-slot rotation\n");
    chipgate_t cg;
    chipgate_init(&cg, CG_GAME_BN5);
    handshake(&cg, CG_ID_PROGRESS);

    /* Record by slot index rather than by frame: the handshake leaves us part
     * way through a rotation, and walking to the top would burn the seed
     * values of the two counters. Nine frames still cover all nine slots. */
    uint16_t seen[9];
    for (int i = 0; i < 9; i++) {
        uint8_t slot = cg.slot;
        seen[slot] = frame(&cg, 0x0000);
    }

    check("slot 0 = gate id",  seen[CG_SLOT_COMMAND], CG_ID_PROGRESS);
    check("slot 1 = continue", seen[CG_SLOT_UNK_0],   CG_WORD_CONTINUE);
    check("slot 2 = continue", seen[CG_SLOT_UNK_1],   CG_WORD_CONTINUE);
    check("slot 3 = data0",    seen[CG_SLOT_DATA_0],  0x00FE);
    check("slot 4 = data1",    seen[CG_SLOT_DATA_1],  0xFFFE);
    check("slot 5 = no chip",  seen[CG_SLOT_ID],      CG_CHIP_NONE);
    check("slot 6 = zero",     seen[CG_SLOT_UNK_2],   0x0000);
    check("slot 7 = zero",     seen[CG_SLOT_UNK_3],   0x0000);
    check("slot 8 = zero",     seen[CG_SLOT_END],     0x0000);
    check("rotation is 9 long", cg.slot, CG_SLOT_UNK_1); /* started here too */

    /* Counters advance once per rotation, not once per frame. */
    for (int i = 0; i < 9; i++) {
        uint8_t slot = cg.slot;
        seen[slot] = frame(&cg, 0x0000);
    }
    check("data0 steps +3",     seen[CG_SLOT_DATA_0], 0x0001); /* (0xFE+3)&0xFF */
    check("data1 steps down",   seen[CG_SLOT_DATA_1], 0xFC00);
}

static void test_chip_send(void) {
    printf("chip transmission\n");
    chipgate_t cg;
    chipgate_init(&cg, CG_GAME_BN6_US);
    handshake(&cg, CG_ID_BEAST_LINK_US);

    const uint16_t kCannon = 0x0001;
    check("load accepted", chipgate_load(&cg, kCannon), 1);
    check("busy", chipgate_busy(&cg), 1);
    check("second load rejected", chipgate_load(&cg, 0x0002), 0);

    int sent = 0;
    for (int f = 0; f < 9 * CG_CHIP_REPEATS + 18; f++) {
        uint8_t slot = cg.slot;
        uint16_t w = frame(&cg, 0x0000);
        if (slot == CG_SLOT_ID && w == kCannon) {
            sent++;
        }
    }

    check("sent for 9 rotations", sent, CG_CHIP_REPEATS);
    check("slot emptied", chipgate_busy(&cg), 0);
    check("consumed edge", chipgate_take_consumed(&cg), 1);
    check("edge is one-shot", chipgate_take_consumed(&cg), 0);
    check("reload now accepted", chipgate_load(&cg, 0x0002), 1);
}

/*
 * The bug behind "device stays busy": a chip is only retired when the rotation
 * reaches its slot, so if the parent stops clocking mid-chip the slot never
 * clears and every later chipgate_load() is refused for ever.
 *
 * The gate cannot fix that by itself -- it has no clock of its own and no
 * notion of time -- so the contract is that chipgate_clear() is the escape,
 * and the caller is responsible for noticing the stall. This pins both halves
 * down so neither can be quietly dropped.
 */
static void test_stalled_link_locks_the_slot(void) {
    printf("a stalled parent locks the chip slot\n");

    chipgate_t cg;
    chipgate_init(&cg, CG_GAME_BN6_US);
    handshake(&cg, CG_ID_BEAST_LINK_US);

    check("chip accepted", chipgate_load(&cg, 0x0001), 1);
    check("busy", chipgate_busy(&cg), 1);

    /* A few transfers, then the parent goes away mid-chip. */
    for (int i = 0; i < 20; i++) {
        frame(&cg, 0x0000);
    }
    check("still busy after a partial rotation", chipgate_busy(&cg), 1);

    /* No amount of waiting helps: without transfers nothing advances. */
    check("still busy however long we wait", chipgate_busy(&cg), 1);
    check("and every further chip is refused", chipgate_load(&cg, 0x0002), 0);

    /* chipgate_clear() is the way out, and it must leave the gate usable. */
    chipgate_clear(&cg);
    check("cleared", chipgate_busy(&cg), 0);
    check("accepts a chip again", chipgate_load(&cg, 0x0002), 1);

    /* And with the parent still clocking, a chip retires on its own. */
    chipgate_t ok;
    chipgate_init(&ok, CG_GAME_BN6_US);
    handshake(&ok, CG_ID_BEAST_LINK_US);
    chipgate_load(&ok, 0x0003);
    for (int i = 0; i < 9 * CG_CHIP_REPEATS + 18; i++) {
        frame(&ok, 0x0000);
    }
    check("retires when the link keeps running", chipgate_busy(&ok), 0);
}

static void test_teardown_and_relink(void) {
    printf("teardown and relink\n");
    chipgate_t cg;
    chipgate_init(&cg, CG_GAME_BN4);
    handshake(&cg, CG_ID_BATTLECHIP);
    check("linked", chipgate_connected(&cg), 1);

    check("ack closes session", frame(&cg, CG_ID_BATTLECHIP), CG_ID_BATTLECHIP);
    check("back to idle", cg.state, CG_LINK_IDLE);

    handshake(&cg, CG_ID_BATTLECHIP);
    check("relinked", chipgate_connected(&cg), 1);
    check("two sessions", cg.sessions, 2);

    /* A chip queued while unlinked survives the reconnect. */
    chipgate_init(&cg, CG_GAME_BN4);
    chipgate_load(&cg, 0x0055);
    handshake(&cg, CG_ID_BATTLECHIP);
    check("chip survived handshake", cg.chip_id, 0x0055);
}

static void test_identities(void) {
    printf("gate identities\n");
    struct { cg_game_t g; uint16_t id; } cases[] = {
        { CG_GAME_BN4,    CG_ID_BATTLECHIP    },
        { CG_GAME_BN5,    CG_ID_PROGRESS      },
        { CG_GAME_BN6_JP, CG_ID_BEAST_LINK    },
        { CG_GAME_BN6_US, CG_ID_BEAST_LINK_US },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        chipgate_t cg;
        chipgate_init(&cg, cases[i].g);
        check(chipgate_game_name(cases[i].g), frame(&cg, 0x0000), cases[i].id);
    }

    chipgate_t cg;
    chipgate_init(&cg, CG_GAME_BN5);
    cg.enabled = false;
    check("disabled goes transparent", frame(&cg, CG_WORD_SYNC), CG_WORD_CONTINUE);
    check("disabled stays idle", cg.state, CG_LINK_IDLE);
}

/* A short readable trace, handy when comparing against an mGBA SIO log. */
static void trace(void) {
    printf("\ntrace: MMBN6 US, Cannon slotted at frame 4\n");
    chipgate_t cg;
    chipgate_init(&cg, CG_GAME_BN6_US);

    const uint16_t script[] = {
        0x0000, CG_WORD_SYNC, CG_ID_BEAST_LINK_US,
        0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    };
    for (unsigned i = 0; i < sizeof script / sizeof script[0]; i++) {
        if (i == 4) {
            chipgate_load(&cg, 0x0001);
        }
        const char *st = chipgate_state_name(cg.state);
        uint8_t slot = cg.slot;
        uint16_t tx = chipgate_step(&cg, script[i]);
        printf("  f%-3u game=%04X  gate=%04X  [%s%s]\n",
               i, script[i], tx, st,
               cg.state == CG_LINK_ACTIVE && slot == CG_SLOT_ID ? " ID" : "");
    }
}

int main(void) {
    test_handshake();
    test_rotation();
    test_chip_send();
    test_stalled_link_locks_the_slot();
    test_teardown_and_relink();
    test_identities();
    trace();

    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
