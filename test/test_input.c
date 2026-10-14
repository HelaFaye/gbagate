/*
 * test_input.c -- the five-way and encoder input paths.
 *
 *   cc -std=c11 -Wall -Wextra -o test_input test_input.c ui_input.c
 *
 * Buttons are where a device feels broken or solid, and almost all of that is
 * timing: a bounce that registers twice, a hold that repeats too eagerly, a
 * long press that also fires the short one. Those are the tests.
 */

#include <stdio.h>
#include <string.h>

#include "ui_input.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-46s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* A switch bank we drive by hand. */
static uint32_t g_raw;
static uint32_t read_raw(void *ctx) { (void)ctx; return g_raw; }

/*
 * Settle a new switch state and return whatever it produces.
 *
 * Debounce needs the reading to hold still across two polls separated by more
 * than the window, so one poll after a change never sees it. Getting that
 * wrong in a test makes the code look broken; it is worth having in one place.
 */
static ui_in_event_t settle(ui_input_t *in, uint32_t *t, uint32_t raw) {
    g_raw = raw;
    ui_input_poll(in, *t);                 /* notice the change */
    *t += UI_IN_DEBOUNCE_MS + 1;
    return ui_input_poll(in, *t);          /* now it is stable  */
}

static void test_debounce(void) {
    printf("debounce\n");

    ui_input_t in;
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);

    uint32_t t = 1000;

    /* A contact bouncing for a few milliseconds must produce one press, not
     * several -- and not one per bounce. */
    for (int i = 0; i < 6; i++) {
        g_raw = (i & 1) ? BTN_DOWN : 0;
        ui_input_poll(&in, t);
        t += 2;
    }
    g_raw = BTN_DOWN;
    int n = 0;
    ui_in_event_t e = { UI_IN_NONE, 0, 0, false };
    for (int i = 0; i < 10; i++) {
        ui_in_event_t x = ui_input_poll(&in, t);
        if (x.kind != UI_IN_NONE && e.kind == UI_IN_NONE) { e = x; n++; }
        else if (x.kind != UI_IN_NONE) n++;
        t += 3;
    }
    check("one press from a bouncing contact", n, 1);
    check("and it is a move", e.kind, UI_IN_MOVE);
    check("downwards", e.value, 1);
}

static void test_repeat(void) {
    printf("auto-repeat\n");

    ui_input_t in;
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);

    uint32_t t = 1000;
    ui_in_event_t e = settle(&in, &t, BTN_DOWN);
    check("press registered", e.kind, UI_IN_MOVE);

    /* Nothing more until the first repeat is due: a deliberate single press
     * must not become two. */
    t += UI_IN_REPEAT_1ST_MS / 2;
    check("silent before the first repeat", ui_input_poll(&in, t).kind, UI_IN_NONE);

    t += UI_IN_REPEAT_1ST_MS;
    check("first repeat", ui_input_poll(&in, t).kind, UI_IN_MOVE);

    /* Then faster, so 400 chips are crossable. */
    t += UI_IN_REPEAT_MS + 1;
    check("second repeat", ui_input_poll(&in, t).kind, UI_IN_MOVE);

    /* Select must never repeat: a held press would toggle a favourite
     * dozens of times. */
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    t = 1000;
    check("select fires once", settle(&in, &t, BTN_SELECT).kind, UI_IN_FAVOURITE);
    t += UI_IN_REPEAT_1ST_MS * 3;
    check("and never repeats", ui_input_poll(&in, t).kind, UI_IN_NONE);
}

static void test_long_press(void) {
    printf("long press\n");

    ui_input_t in;
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    uint32_t t = 1000;

    /* Short press on A: nothing on the edge, the select arrives on release.
     * Firing on the edge as well would send both meanings for one press. */
    check("nothing yet while held", settle(&in, &t, BTN_A).kind, UI_IN_NONE);
    check("select on release", settle(&in, &t, 0).kind, UI_IN_SELECT);

    /* Long press on A opens sort, and the release must not also select. */
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    t = 1000;
    settle(&in, &t, BTN_A);
    t += UI_IN_LONG_MS + 1;
    check("sort on the hold", ui_input_poll(&in, t).kind, UI_IN_SORT);
    check("release adds nothing", settle(&in, &t, 0).kind, UI_IN_NONE);
}

static void test_mapping(void) {
    printf("the five-way mapping\n");

    ui_input_t in;
    uint32_t t;

    struct { uint32_t btn; ui_in_kind_t kind; int32_t value; } cases[] = {
        { BTN_UP,     UI_IN_MOVE, -1 },
        { BTN_DOWN,   UI_IN_MOVE, +1 },
        { BTN_LEFT,   UI_IN_TAB,  -1 },
        { BTN_RIGHT,  UI_IN_TAB,  +1 },
        { BTN_B,      UI_IN_BACK,  0 },
        { BTN_SELECT, UI_IN_FAVOURITE, 0 },
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
        t = 1000;
        ui_in_event_t e = settle(&in, &t, cases[i].btn);
        if (e.kind != cases[i].kind || e.value != cases[i].value) {
            printf("  FAIL button %u gave kind %d value %d\n",
                   cases[i].btn, e.kind, e.value);
            bad++;
        }
    }
    /* Count this test's own failures, not the global tally -- otherwise an
     * earlier failure makes this one report a fault it did not find. */
    check("all six map correctly", bad, 0);
}

static void test_two_versus_four(void) {
    printf("two switches versus four\n");

    ui_input_t in;
    uint32_t t = 1000;

    /*
     * With two switches, L and R do not exist. Their pins are not read, so a
     * floating input -- which on a board with pull-ups reads as pressed --
     * must not become a page jump.
     */
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    check("L and R ignored on a 2-switch board",
          settle(&in, &t, BTN_L | BTN_R).kind, UI_IN_NONE);

    /* With four, they page the list. */
    ui_input_init(&in, INPUT_DPAD4, read_raw, NULL);
    t = 1000;
    ui_in_event_t e = settle(&in, &t, BTN_R);
    check("R pages forward", e.kind, UI_IN_MOVE);
    check("by a screenful", e.value, UI_IN_PAGE);

    ui_input_init(&in, INPUT_DPAD4, read_raw, NULL);
    t = 1000;
    check("L pages back", settle(&in, &t, BTN_L).value, -UI_IN_PAGE);

    check("switch counts", input_mode_switches(INPUT_DPAD2), 7);
    check("and for four", input_mode_switches(INPUT_DPAD4), 9);
}

static void test_encoder_mode(void) {
    printf("encoder mode\n");

    /*
     * With one control there is no B, so the long press has to be back rather
     * than sort -- the opposite of the five-way. Getting this backwards would
     * leave an encoder-only board with no way out of a mode.
     */
    ui_input_t in;
    ui_input_init(&in, INPUT_ENCODER, read_raw, NULL);
    uint32_t t = 1000;

    settle(&in, &t, BTN_A);
    t += UI_IN_LONG_MS + 1;
    check("long press is back on an encoder", ui_input_poll(&in, t).kind, UI_IN_BACK);

    /* Direction buttons do not exist here and must be ignored. */
    ui_input_init(&in, INPUT_ENCODER, read_raw, NULL);
    t = 1000;
    check("no phantom directions",
          settle(&in, &t, BTN_UP | BTN_LEFT | BTN_SELECT).kind, UI_IN_NONE);
    check("one switch", input_mode_switches(INPUT_ENCODER), 1);
}

/*
 * The fifth switch. Start takes over sort, and the interesting consequence is
 * what that does to A rather than what it does to sort.
 */
static void test_five_switches(void) {
    printf("five switches: Start, and a faster A\n");

    ui_input_t in;
    uint32_t t = 1000;

    /* Start is sort, directly. */
    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL);
    check("Start opens sort", settle(&in, &t, BTN_START).kind, UI_IN_SORT);

    /*
     * And A now fires on the edge instead of on release. On a two- or
     * four-switch board it has to wait, because until the button comes up the
     * press might still become a long one. Here it cannot, so it does not.
     */
    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL);
    t = 1000;
    check("A selects while still held", settle(&in, &t, BTN_A).kind, UI_IN_SELECT);
    check("and releasing adds nothing", settle(&in, &t, 0).kind, UI_IN_NONE);

    /* Contrast: on four switches the same press reports nothing until release. */
    ui_input_init(&in, INPUT_DPAD4, read_raw, NULL);
    t = 1000;
    check("four switches: A waits", settle(&in, &t, BTN_A).kind, UI_IN_NONE);
    check("and selects on release", settle(&in, &t, 0).kind, UI_IN_SELECT);

    /* Holding A no longer opens sort, since Start does. */
    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL);
    t = 1000;
    settle(&in, &t, BTN_A);
    t += UI_IN_LONG_MS * 2;
    check("holding A does nothing more", ui_input_poll(&in, t).kind, UI_IN_NONE);

    /* Start must not exist on the smaller boards: an unread pin cannot open a
     * sort menu the user did not ask for. */
    ui_input_init(&in, INPUT_DPAD4, read_raw, NULL);
    t = 1000;
    check("no Start on four switches",
          settle(&in, &t, BTN_START).kind, UI_IN_NONE);
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    t = 1000;
    check("nor on two", settle(&in, &t, BTN_START).kind, UI_IN_NONE);

    check("ten switches", input_mode_switches(INPUT_DPAD5), 10);

    /* Long-press A still means sort where there is no Start, and back on the
     * encoder. All three modes differ here and all three matter. */
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    t = 1000; settle(&in, &t, BTN_A); t += UI_IN_LONG_MS + 1;
    check("two switches: hold is sort", ui_input_poll(&in, t).kind, UI_IN_SORT);

    ui_input_init(&in, INPUT_ENCODER, read_raw, NULL);
    t = 1000; settle(&in, &t, BTN_A); t += UI_IN_LONG_MS + 1;
    check("encoder: hold is back", ui_input_poll(&in, t).kind, UI_IN_BACK);
}

/*
 * Long presses belong to the screen, not to the board.
 *
 * They used to be decided by mode, which made A's timing depend on which
 * hardware was fitted -- so nothing above this layer could rely on long-A
 * existing, and a screen that used it worked on one board and not another.
 */
static void test_long_press_is_per_context(void) {
    printf("long press is a property of the context\n");

    ui_input_t in;
    uint32_t t;

    /* The defaults still match the modes, so a caller that never asks keeps
     * the sensible behaviour. */
    check("encoder holds A by default",
          ui_input_default_long(INPUT_ENCODER), BTN_A);
    check("two switches too", ui_input_default_long(INPUT_DPAD2), BTN_A);
    check("five switches need no hold", ui_input_default_long(INPUT_DPAD5), 0);

    /*
     * A screen on a five-switch board can ask for long-A anyway. This is the
     * case that could not be expressed before: the board has Start, so the
     * default is edge response, but this screen wants a hold.
     */
    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL);
    ui_input_want_long(&in, BTN_A);
    t = 1000;
    check("A now waits", settle(&in, &t, BTN_A).kind, UI_IN_NONE);
    t += UI_IN_LONG_MS + 1;
    check("and holds", ui_input_poll(&in, t).kind, UI_IN_SORT);

    /*
     * And a screen on a two-switch board can give the latency back where it
     * has no use for a hold -- the responsiveness is no longer reserved for
     * boards with ten switches.
     */
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    ui_input_want_long(&in, 0);
    t = 1000;
    check("A fires on the edge", settle(&in, &t, BTN_A).kind, UI_IN_SELECT);
    check("and release adds nothing", settle(&in, &t, 0).kind, UI_IN_NONE);

    /* Any button can have one, not just A. */
    ui_input_init(&in, INPUT_DPAD4, read_raw, NULL);
    ui_input_want_long(&in, BTN_B);
    t = 1000;
    check("B waits when asked", settle(&in, &t, BTN_B).kind, UI_IN_NONE);
    t += UI_IN_LONG_MS + 1;
    check("B holds", ui_input_poll(&in, t).kind, UI_IN_SORT);

    /*
     * Changing the mask mid-press must not judge a press by a rule it did not
     * begin under: releasing here would otherwise emit a select the new screen
     * never armed.
     */
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    t = 1000;
    settle(&in, &t, BTN_A);           /* pressed under "A holds" */
    ui_input_want_long(&in, 0);       /* screen changes */
    check("the in-flight press is dropped", settle(&in, &t, 0).kind, UI_IN_NONE);

    /* Setting the same mask again must not drop a press that is legitimately
     * in flight. */
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    t = 1000;
    settle(&in, &t, BTN_A);
    ui_input_want_long(&in, BTN_A);   /* no change */
    check("an unchanged mask leaves it alone",
          settle(&in, &t, 0).kind, UI_IN_SELECT);
}

int main(void) {
    test_debounce();
    test_repeat();
    test_long_press();
    test_mapping();
    test_two_versus_four();
    test_encoder_mode();
    test_five_switches();
    test_long_press_is_per_context();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
