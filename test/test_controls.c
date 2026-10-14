/*
 * test_controls.c -- intent, hardware, and the hint that describes both.
 *
 *   cc -std=c11 -Wall -Wextra -o test_controls test_controls.c controls.c ui_input.c
 *
 * The point of this layer is that a screen says what it does and never names a
 * button. So the tests are mostly: does the same declaration produce sensible
 * and *honest* behaviour on four different sets of hardware.
 */

#include <stdio.h>
#include <string.h>

#include "controls.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-46s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static void check_str(const char *what, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        printf("  FAIL %-46s got \"%s\", want \"%s\"\n",
               what, got ? got : "(null)", want);
        g_fail++;
    }
}

static uint32_t g_raw;
static uint32_t read_raw(void *ctx) { (void)ctx; return g_raw; }

/* A chip screen: everything it can use. */
#define CHIP_SCREEN (CTRL_BIT(CTRL_PREV) | CTRL_BIT(CTRL_NEXT) | \
                     CTRL_BIT(CTRL_PAGE_PREV) | CTRL_BIT(CTRL_PAGE_NEXT) | \
                     CTRL_BIT(CTRL_TAB_PREV) | CTRL_BIT(CTRL_TAB_NEXT) | \
                     CTRL_BIT(CTRL_CONFIRM) | CTRL_BIT(CTRL_CANCEL) | \
                     CTRL_BIT(CTRL_MARK) | CTRL_BIT(CTRL_MENU))

static ctrl_action_t settle(controls_t *c, uint32_t *t, uint32_t raw,
                            int32_t *value) {
    g_raw = raw;
    controls_next(c, *t, value);
    *t += UI_IN_DEBOUNCE_MS + 1;
    return controls_next(c, *t, value);
}

static void test_same_screen_four_boards(void) {
    printf("one declaration, four sets of hardware\n");

    struct { input_mode_t mode; const char *name; } boards[] = {
        { INPUT_ENCODER, "encoder" }, { INPUT_DPAD2, "5-way+2" },
        { INPUT_DPAD4, "5-way+4" },   { INPUT_DPAD5, "5-way+5" },
    };

    for (unsigned i = 0; i < 4; i++) {
        ui_input_t in;
        controls_t c;
        ui_input_init(&in, boards[i].mode, read_raw, NULL);
        controls_bind(&c, &in, CHIP_SCREEN);

        /* Confirm and cancel must be reachable on every board, or the screen
         * is a trap: something you can enter and not leave. */
        if (!controls_available(&c, CTRL_CONFIRM)) {
            printf("  FAIL %s cannot confirm\n", boards[i].name);
            g_fail++;
        }
        if (!controls_available(&c, CTRL_CANCEL)) {
            printf("  FAIL %s cannot cancel\n", boards[i].name);
            g_fail++;
        }
        printf("    %-9s confirm=%-10s cancel=%-10s menu=%s\n",
               boards[i].name,
               controls_label(&c, CTRL_CONFIRM),
               controls_label(&c, CTRL_CANCEL),
               controls_label(&c, CTRL_MENU) ? controls_label(&c, CTRL_MENU)
                                             : "(unreachable)");
    }
    check("every board can confirm and cancel", g_fail, 0);
}

static void test_unreachable_is_reported(void) {
    printf("what the hardware cannot do\n");

    ui_input_t in;
    controls_t c;

    /*
     * An encoder has one button. Marking a favourite and opening a sort menu
     * have nowhere to go, and the honest answer is to say so -- a screen can
     * then hide the function instead of showing one that cannot be invoked.
     */
    ui_input_init(&in, INPUT_ENCODER, read_raw, NULL);
    controls_bind(&c, &in, CHIP_SCREEN);
    check("encoder cannot mark", controls_available(&c, CTRL_MARK), 0);
    check("encoder cannot open a menu", controls_available(&c, CTRL_MENU), 0);
    check("and says so with no label", controls_label(&c, CTRL_MARK) == NULL, 1);

    /* Two switches have no L/R, so no paging. */
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    controls_bind(&c, &in, CHIP_SCREEN);
    check("two switches cannot page", controls_available(&c, CTRL_PAGE_NEXT), 0);
    check("but can mark", controls_available(&c, CTRL_MARK), 1);

    ui_input_init(&in, INPUT_DPAD4, read_raw, NULL);
    controls_bind(&c, &in, CHIP_SCREEN);
    check("four switches can page", controls_available(&c, CTRL_PAGE_NEXT), 1);
}

static void test_hint_matches_the_hardware(void) {
    printf("the hint describes the fitted board\n");

    ui_input_t in;
    controls_t c;

    /*
     * This is why the layer exists. The footers were literals -- one read
     * "back: backspace", naming a key on the QEMU viewer's keyboard. A hint
     * built from the same table that dispatches the press cannot say that.
     */
    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL);
    controls_bind(&c, &in, CHIP_SCREEN);
    check_str("five switches", controls_hint(&c),
              "A send  B back  Select fav  Start sort");

    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    controls_bind(&c, &in, CHIP_SCREEN);
    check_str("two switches: sort borrows a hold", controls_hint(&c),
              "A send  B back  Select fav  hold A sort");

    /* The encoder names what it has, and omits what it does not. */
    ui_input_init(&in, INPUT_ENCODER, read_raw, NULL);
    controls_bind(&c, &in, CHIP_SCREEN);
    check_str("encoder", controls_hint(&c), "click send  hold click back");

    /* A screen that uses less says less. */
    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL);
    controls_bind(&c, &in, CTRL_BIT(CTRL_CONFIRM) | CTRL_BIT(CTRL_CANCEL));
    check_str("a simpler screen", controls_hint(&c), "A send  B back");
}

static void test_long_press_is_derived(void) {
    printf("long presses follow from the declaration\n");

    ui_input_t in;
    controls_t c;

    /*
     * A screen that uses the menu needs a hold on two switches, and pays the
     * latency. A screen that does not should not pay it -- and no screen
     * should have to reason about that.
     */
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    controls_bind(&c, &in, CHIP_SCREEN);
    check("menu screen arms a hold on A", in.long_mask, BTN_A);

    controls_bind(&c, &in, CTRL_BIT(CTRL_CONFIRM) | CTRL_BIT(CTRL_CANCEL));
    check("a screen without a menu does not", in.long_mask, 0);

    /* On five switches even the menu screen needs no hold, because Start is
     * a button of its own. */
    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL);
    controls_bind(&c, &in, CHIP_SCREEN);
    check("five switches never need a hold", in.long_mask, 0);
}

static void test_dispatch(void) {
    printf("presses become intent\n");

    ui_input_t in;
    controls_t c;
    uint32_t t = 1000;
    int32_t v;

    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL);
    controls_bind(&c, &in, CHIP_SCREEN);

    check("down is next", settle(&c, &t, BTN_DOWN, &v), CTRL_NEXT);
    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL); controls_bind(&c, &in, CHIP_SCREEN); t = 1000;
    check("right is a tab", settle(&c, &t, BTN_RIGHT, &v), CTRL_TAB_NEXT);
    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL); controls_bind(&c, &in, CHIP_SCREEN); t = 1000;
    check("Start is the menu", settle(&c, &t, BTN_START, &v), CTRL_MENU);
    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL); controls_bind(&c, &in, CHIP_SCREEN); t = 1000;
    check("R pages", settle(&c, &t, BTN_R, &v), CTRL_PAGE_NEXT);
    check("by a screenful", v, UI_IN_PAGE);

    /*
     * A press the screen did not ask for must not reach it. Otherwise every
     * screen filters by hand, and the ones that forget behave oddly.
     */
    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL);
    controls_bind(&c, &in, CTRL_BIT(CTRL_CONFIRM) | CTRL_BIT(CTRL_CANCEL));
    t = 1000;
    check("unwanted press dropped", settle(&c, &t, BTN_SELECT, &v), CTRL_NONE);
    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL);
    controls_bind(&c, &in, CTRL_BIT(CTRL_CONFIRM) | CTRL_BIT(CTRL_CANCEL));
    t = 1000;
    check("wanted press still arrives", settle(&c, &t, BTN_A, &v), CTRL_CONFIRM);
}

/*
 * Content -- a mode, or an applet loaded into one -- asking for a hold of its
 * own. The interesting part is not that it works, but what happens when the
 * shell already wants the same control.
 */
static void test_content_hold(void) {
    printf("a hold belonging to loaded content\n");

    ui_input_t in;
    controls_t c;
    uint32_t t;
    int32_t v;

    uint32_t screen = CTRL_BIT(CTRL_CONFIRM) | CTRL_BIT(CTRL_CANCEL);

    /*
     * Five switches: Start owns sort, so A's hold is free and content gets it.
     * Both the menu and the content hold are available at once.
     */
    ui_input_init(&in, INPUT_DPAD5, read_raw, NULL);
    controls_bind(&c, &in, screen | CTRL_BIT(CTRL_MENU) | CTRL_BIT(CTRL_ALT));
    check("five switches: menu available", controls_available(&c, CTRL_MENU), 1);
    check("and the content hold too", controls_available(&c, CTRL_ALT), 1);
    check("no conflict", controls_conflicts(&c), 0);
    check_str("menu is Start", controls_label(&c, CTRL_MENU), "Start");
    check_str("content gets hold A", controls_label(&c, CTRL_ALT), "hold A");
    check("so A is armed for holding", in.long_mask, BTN_A);

    /* And it actually fires. */
    t = 1000;
    controls_next(&c, t, &v);
    g_raw = BTN_A;
    controls_next(&c, t, &v);
    t += UI_IN_DEBOUNCE_MS + 1;
    controls_next(&c, t, &v);
    t += UI_IN_LONG_MS + 1;
    check("holding A reaches the content", controls_next(&c, t, &v), CTRL_ALT);

    /*
     * Two switches: sort already wants hold-A. Something has to lose, and the
     * enum order decides -- sort keeps it, content is told no. Reported rather
     * than silently dropped, which is what calling ui_input_want_long() by
     * hand would have done.
     */
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    controls_bind(&c, &in, screen | CTRL_BIT(CTRL_MENU) | CTRL_BIT(CTRL_ALT));
    check("two switches: sort keeps the hold", controls_available(&c, CTRL_MENU), 1);
    check("content does not get one", controls_available(&c, CTRL_ALT), 0);
    check("and the clash is reported", controls_conflicts(&c), CTRL_BIT(CTRL_ALT));
    check("no label for it", controls_label(&c, CTRL_ALT) == NULL, 1);

    /*
     * The same board, for content that does not use sort: now the hold is
     * free and content has it. Availability depends on what else is asked
     * for, not only on the hardware.
     */
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    controls_bind(&c, &in, screen | CTRL_BIT(CTRL_ALT));
    check("without sort, content gets the hold",
          controls_available(&c, CTRL_ALT), 1);
    check("no conflict", controls_conflicts(&c), 0);
    check_str("labelled honestly", controls_label(&c, CTRL_ALT), "hold A");

    /*
     * The encoder has one button and its hold is the only way back. Content
     * asking for it is asking to strand the user, so it does not get it.
     */
    ui_input_init(&in, INPUT_ENCODER, read_raw, NULL);
    controls_bind(&c, &in, screen | CTRL_BIT(CTRL_ALT));
    check("encoder keeps back", controls_available(&c, CTRL_CANCEL), 1);
    check("content refused", controls_available(&c, CTRL_ALT), 0);
    check("reported", controls_conflicts(&c), CTRL_BIT(CTRL_ALT));
}

/*
 * The sets gate_app.c binds, on every board. These are the strings that end up
 * on the footer, and the whole reason this layer exists is that they used to
 * be literals -- one of which named a key on the QEMU viewer's keyboard.
 */
static void test_screen_sets(void) {
    printf("what the real screens will show\n");

    #define CTRLS_PAGE   (CTRL_BIT(CTRL_CONFIRM) | CTRL_BIT(CTRL_CANCEL))
    #define CTRLS_LIST   (CTRLS_PAGE | CTRL_BIT(CTRL_PREV) | CTRL_BIT(CTRL_NEXT) | \
                          CTRL_BIT(CTRL_PAGE_PREV) | CTRL_BIT(CTRL_PAGE_NEXT))
    #define CTRLS_CHIPS  (CTRLS_LIST | CTRL_BIT(CTRL_TAB_PREV) | \
                          CTRL_BIT(CTRL_TAB_NEXT) | CTRL_BIT(CTRL_MARK) | \
                          CTRL_BIT(CTRL_MENU))

    struct { input_mode_t mode; const char *name; } boards[] = {
        { INPUT_ENCODER, "encoder" }, { INPUT_DPAD2, "5-way+2" },
        { INPUT_DPAD4, "5-way+4" },   { INPUT_DPAD5, "5-way+5" },
    };

    for (unsigned i = 0; i < 4; i++) {
        ui_input_t in;
        controls_t c;
        ui_input_init(&in, boards[i].mode, read_raw, NULL);

        controls_bind(&c, &in, CTRLS_CHIPS);
        printf("    %-9s chips: %s\n", boards[i].name, controls_hint(&c));
        controls_bind(&c, &in, CTRLS_PAGE);
        printf("    %-9s page:  %s\n", boards[i].name, controls_hint(&c));

        /* No hint may name a key that is not on the board. "enter" and
         * "backspace" were the literals this replaced. */
        controls_bind(&c, &in, CTRLS_CHIPS);
        const char *h = controls_hint(&c);
        if (strstr(h, "enter") || strstr(h, "backspace")) {
            printf("  FAIL %s hint names a keyboard key: %s\n",
                   boards[i].name, h);
            g_fail++;
        }
        /* And every board's page hint must offer a way back, or a screen can
         * be entered and not left. */
        controls_bind(&c, &in, CTRLS_PAGE);
        if (!controls_available(&c, CTRL_CANCEL)) {
            printf("  FAIL %s page has no way back\n", boards[i].name);
            g_fail++;
        }
    }

    /*
     * A page screen never needs a hold: it has no menu, so confirm stays on
     * the edge even where sort would have borrowed A.
     */
    ui_input_t in;
    controls_t c;
    ui_input_init(&in, INPUT_DPAD2, read_raw, NULL);
    controls_bind(&c, &in, CTRLS_PAGE);
    check("a page screen arms no hold", in.long_mask, 0);
    controls_bind(&c, &in, CTRLS_CHIPS);
    check("the chip screen does", in.long_mask, BTN_A);
}

int main(void) {
    test_same_screen_four_boards();
    test_unreachable_is_reported();
    test_hint_matches_the_hardware();
    test_long_press_is_derived();
    test_dispatch();
    test_content_hold();
    test_screen_sets();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
