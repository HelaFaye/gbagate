/*
 * test_encoder.c -- rotary encoder decode, acceleration and button.
 *
 *   cc -std=c11 -Wall -Wextra -o test_encoder test_encoder.c input_encoder.c ui_list.c
 */

#include <stdio.h>
#include <string.h>

#include "input_encoder.h"
#include "ui_list.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-40s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* One full detent clockwise: 00 -> 01 -> 11 -> 10 -> 00 */
static const uint8_t CW[4]  = { 1, 3, 2, 0 };
static const uint8_t CCW[4] = { 2, 3, 1, 0 };

static int32_t turn(encoder_t *e, const uint8_t *seq, int detents,
                    uint32_t *t, uint32_t step_ms) {
    int32_t total = 0;
    for (int d = 0; d < detents; d++) {
        for (int i = 0; i < 4; i++) {
            uint8_t s = seq[i];
            *t += step_ms;
            encoder_event_t ev = encoder_poll(e, (s & 2) != 0, (s & 1) != 0, false, *t);
            total += ev.delta;
        }
    }
    return total;
}

static void settle(encoder_t *e, uint32_t *t) {
    for (int i = 0; i < 4; i++) {
        *t += 1;
        encoder_poll(e, false, false, false, *t);
    }
}

static void test_direction(void) {
    printf("direction\n");
    encoder_t e;
    uint32_t t = 1000;

    encoder_init(&e, true);
    settle(&e, &t);
    check("one detent CW", turn(&e, CW, 1, &t, 50), 1);
    check("detent counted", e.detents, 1);

    encoder_init(&e, true);
    t = 1000;
    settle(&e, &t);
    check("one detent CCW", turn(&e, CCW, 1, &t, 50), -1);

    /* Slow deliberate turns must always be exactly one row. */
    encoder_init(&e, true);
    t = 1000;
    settle(&e, &t);
    check("five slow detents", turn(&e, CW, 5, &t, 100), 5);
    check("no glitches on clean input", e.glitches, 0);
}

static void test_bounce(void) {
    printf("contact bounce\n");
    encoder_t e;
    uint32_t t = 1000;
    encoder_init(&e, true);
    settle(&e, &t);

    /* Chatter on the first edge: 00 -> 01 -> 00 -> 01 then complete the detent.
     * The legal-transition table makes these cancel rather than accumulate. */
    const uint8_t chatter[] = { 1, 0, 1, 0, 1, 3, 2, 0 };
    int32_t total = 0;
    for (unsigned i = 0; i < sizeof chatter; i++) {
        uint8_t s = chatter[i];
        t += 5;
        encoder_event_t ev = encoder_poll(&e, (s & 2) != 0, (s & 1) != 0, false, t);
        total += ev.delta;
    }
    check("bounce yields exactly one detent", total, 1);

    /* An illegal double transition (00 -> 11) must be rejected, not guessed. */
    encoder_init(&e, true);
    t = 1000;
    settle(&e, &t);
    t += 5;
    encoder_poll(&e, true, true, false, t);
    check("illegal transition counted", e.glitches, 1);
    check("no movement from a glitch", e.detents, 0);
}

static void test_acceleration(void) {
    printf("acceleration\n");
    encoder_t e;
    encoder_init(&e, true);

    check("slow turn is 1:1", encoder_accel_step(&e, 500), 1);
    check("at the window edge is 1:1", encoder_accel_step(&e, ENC_ACCEL_WINDOW_MS), 1);
    check("fastest is capped", encoder_accel_step(&e, 0), ENC_ACCEL_MAX);
    check("halfway is between", encoder_accel_step(&e, ENC_ACCEL_WINDOW_MS / 2),
          1 + (ENC_ACCEL_MAX - 1) / 2);

    /* Monotonic: faster must never move less. */
    uint8_t prev = 0;
    for (int gap = ENC_ACCEL_WINDOW_MS; gap >= 0; gap -= 10) {
        uint8_t s = encoder_accel_step(&e, (uint32_t)gap);
        if (s < prev) {
            check("monotonic in speed", 0, 1);
            break;
        }
        prev = s;
    }
}

/* The point of all this: can you actually cross the BN6 library? */
static void test_traversal(void) {
    printf("crossing 411 chips\n");

    static uint16_t items[411];
    for (uint16_t i = 0; i < 411; i++) {
        items[i] = i;
    }
    ui_list_t l;
    ui_list_init(&l, UI_ROWS, true);
    ui_list_set(&l, items, 411);

    encoder_t e;
    encoder_init(&e, true);
    uint32_t t = 1000;
    settle(&e, &t);

    int detents = 0;
    while (l.cursor < 400 && detents < 200) {
        int32_t d = turn(&e, CW, 1, &t, 5); /* fast spin: 20 ms per detent */
        ui_list_move(&l, d);
        detents++;
    }
    printf("  reached item %u in %d detents\n", l.cursor, detents);
    check("fast spin crosses the library", detents < 40, 1);

    /* And a slow turn still lands exactly. */
    ui_list_goto(&l, 100);
    encoder_init(&e, true);
    t += 5000;
    settle(&e, &t);
    for (int i = 0; i < 3; i++) {
        ui_list_move(&l, turn(&e, CW, 1, &t, 100));
    }
    check("slow turn is exact", l.cursor, 103);
}

static void test_button(void) {
    printf("button\n");
    encoder_t e;
    encoder_init(&e, true); /* active low: pressed == level 0 */
    uint32_t t = 1000;

    encoder_event_t ev;
    for (int i = 0; i < 5; i++) {
        t += 1;
        ev = encoder_poll(&e, false, false, true, t); /* not pressed */
    }
    check("idle: no press", ev.pressed, 0);

    for (int i = 0; i < 3; i++) {
        t += 1;
        ev = encoder_poll(&e, false, false, false, t);
    }
    check("press detected", ev.pressed, 1);

    t += 50;
    ev = encoder_poll(&e, false, false, false, t);
    check("no long press yet", ev.long_press, 0);

    for (int i = 0; i < 3; i++) {
        t += 1;
        ev = encoder_poll(&e, false, false, true, t);
    }
    check("release", ev.released, 1);
    check("counts as a click", ev.clicked, 1);
    check("not a long press", ev.long_press, 0);

    /* Hold past the threshold: long press fires once, and the release must not
     * also register as a click. */
    encoder_init(&e, true);
    t = 1000;
    for (int i = 0; i < 3; i++) {
        t += 1;
        encoder_poll(&e, false, false, false, t);
    }
    t += ENC_LONG_PRESS_MS;
    ev = encoder_poll(&e, false, false, false, t);
    check("long press fires", ev.long_press, 1);

    t += 100;
    ev = encoder_poll(&e, false, false, false, t);
    check("fires only once", ev.long_press, 0);

    for (int i = 0; i < 3; i++) {
        t += 1;
        ev = encoder_poll(&e, false, false, true, t);
    }
    check("released", ev.released, 1);
    check("hold is not also a click", ev.clicked, 0);

    /* Button bounce must not produce phantom presses. */
    encoder_init(&e, true);
    t = 1000;
    int presses = 0;
    const bool chatter[] = { true, false, true, false, false, false, false, false };
    for (unsigned i = 0; i < sizeof chatter / sizeof chatter[0]; i++) {
        t += 1;
        ev = encoder_poll(&e, false, false, chatter[i], t);
        if (ev.pressed) {
            presses++;
        }
    }
    check("bounce gives one press", presses, 1);
}

int main(void) {
    test_direction();
    test_bounce();
    test_acceleration();
    test_traversal();
    test_button();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
