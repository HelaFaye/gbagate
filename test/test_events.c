/*
 * test_events.c -- from a control intent to a UI event (src/input_events.c).
 *
 * The hardware input task and the QEMU keyboard both go through this, so
 * what it maps is what every board does. The gap it closes: tabs, the
 * favourite mark and sort reached no screen on any board.
 */

#include <stdio.h>
#include <string.h>

#include "input_events.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-46s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static void test_mapping(void) {
    printf("mapping\n");
    const struct { ctrl_action_t a; int32_t v; ui_event_type_t t; int32_t ea; } k[] = {
        { CTRL_PREV,      0,  UI_EVENT_ENCODER,   -1 },
        { CTRL_NEXT,      0,  UI_EVENT_ENCODER,   +1 },
        { CTRL_PREV,     -5,  UI_EVENT_ENCODER,   -5 },  /* an accelerated turn */
        { CTRL_NEXT,      7,  UI_EVENT_ENCODER,    7 },
        { CTRL_PAGE_PREV, -8, UI_EVENT_ENCODER,   -8 },
        { CTRL_PAGE_NEXT, 8,  UI_EVENT_ENCODER,    8 },
        { CTRL_CONFIRM,   0,  UI_EVENT_CHIP_SENT,  0 },
        { CTRL_CANCEL,    0,  UI_EVENT_CHIP_OUT,   0 },
        { CTRL_TAB_PREV,  0,  UI_EVENT_TAB,       -1 },
        { CTRL_TAB_NEXT,  0,  UI_EVENT_TAB,       +1 },
        { CTRL_MARK,      0,  UI_EVENT_MARK,       0 },
        { CTRL_MENU,      0,  UI_EVENT_MENU,       0 },
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        ui_event_t e;
        char what[48];
        snprintf(what, sizeof what, "action %d mapped", (int)k[i].a);
        check(what, input_event_for(k[i].a, k[i].v, &e), 1);
        snprintf(what, sizeof what, "action %d type", (int)k[i].a);
        check(what, e.type, k[i].t);
        snprintf(what, sizeof what, "action %d value", (int)k[i].a);
        check(what, e.a, k[i].ea);
    }
    ui_event_t e;
    check("none is not an event", input_event_for(CTRL_NONE, 0, &e), 0);
    check("nor is alt", input_event_for(CTRL_ALT, 0, &e), 0);
    check("NULL out is fine", input_event_for(CTRL_CONFIRM, 0, NULL), 1);

    /* Every action a screen can ask for has an event. */
    for (int a = 1; a < CTRL_ACTION_COUNT; a++) {
        if (INPUT_WANTED & CTRL_BIT(a)) {
            char what[48];
            snprintf(what, sizeof what, "wanted action %d has an event", a);
            check(what, input_event_for((ctrl_action_t)a, 0, NULL), 1);
        }
    }
}

/* ---- draining a real ui_input through controls -------------------------- */

static uint32_t g_raw;
static uint32_t read_raw(void *ctx) { (void)ctx; return g_raw; }

static ui_event_t g_got[64];
static int        g_n;
static bool        g_accept = true;
static bool post(const ui_event_t *e) {
    if (!g_accept) return false;
    if (g_n < 64) g_got[g_n++] = *e;
    return true;
}

static int count(ui_event_type_t t) {
    int n = 0;
    for (int i = 0; i < g_n; i++) n += g_got[i].type == t;
    return n;
}

/* Press and release a button through debounce, draining as the task does. */
static uint32_t g_t;
static void press(uint32_t bit, uint32_t hold_ms, controls_t *c) {
    g_raw = bit;
    for (uint32_t i = 0; i < hold_ms; i++) input_drain(c, ++g_t, post);
    g_raw = 0;
    for (uint32_t i = 0; i < 40; i++) input_drain(c, ++g_t, post);
}

static void test_drain(void) {
    printf("drain\n");
    ui_input_t in;
    controls_t c;

    /* A five-way with four switches: every action has its own button. */
    ui_input_init(&in, INPUT_DPAD4, read_raw, NULL);
    controls_bind(&c, &in, INPUT_WANTED);
    g_n = 0;
    press(BTN_A, 30, &c);
    check("A confirms", count(UI_EVENT_CHIP_SENT), 1);
    press(BTN_B, 30, &c);
    check("B goes back", count(UI_EVENT_CHIP_OUT), 1);
    /* Left and right change tab; L and R page (controls.c's table). */
    press(BTN_RIGHT, 30, &c);
    press(BTN_LEFT, 30, &c);
    check("left and right change tab", count(UI_EVENT_TAB), 2);
    bool next = false, prev = false;
    for (int i = 0; i < g_n; i++) {
        if (g_got[i].type == UI_EVENT_TAB) {
            next |= g_got[i].a > 0;
            prev |= g_got[i].a < 0;
        }
    }
    check("right is next", next, 1);
    check("left is previous", prev, 1);
    g_n = 0;
    press(BTN_R, 30, &c);
    check("R pages", count(UI_EVENT_ENCODER), 1);
    check("a page forward", g_got[0].a >= UI_IN_PAGE, 1);
    press(BTN_DOWN, 30, &c);
    check("down moves one", g_n == 2 && g_got[1].type == UI_EVENT_ENCODER &&
          g_got[1].a == 1, 1);
    press(BTN_SELECT, 30, &c);
    check("a favourite mark arrives", count(UI_EVENT_MARK), 1);

    /* The encoder's button: a click confirms, a hold goes back. */
    ui_input_init(&in, INPUT_ENCODER, read_raw, NULL);
    controls_bind(&c, &in, INPUT_WANTED);
    g_n = 0;
    press(BTN_A, 30, &c);
    check("encoder click confirms", count(UI_EVENT_CHIP_SENT), 1);
    press(BTN_A, UI_IN_LONG_MS + 50, &c);
    check("encoder hold goes back", count(UI_EVENT_CHIP_OUT), 1);
    check("and does not also confirm", count(UI_EVENT_CHIP_SENT), 1);

    /* A full queue: nothing counted as posted. */
    g_accept = false;
    g_n = 0;
    ui_input_init(&in, INPUT_DPAD4, read_raw, NULL);
    controls_bind(&c, &in, INPUT_WANTED);
    g_raw = BTN_A;
    size_t posted = 0;
    for (int i = 0; i < 30; i++) posted += input_drain(&c, ++g_t, post);
    g_raw = 0;
    for (int i = 0; i < 40; i++) posted += input_drain(&c, ++g_t, post);
    check("refused posts are not counted", (long)posted, 0);
    g_accept = true;
    check("no post function is safe", (long)input_drain(&c, ++g_t, NULL), 0);
}

int main(void) {
    test_mapping();
    test_drain();
    if (g_fail) {
        printf("%d FAILURES\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
