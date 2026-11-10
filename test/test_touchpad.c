/*
 * test_touch_pad.c -- the touch carrier's panel layout, the 15/8 scale, the
 * touch calibration and the key strips (src/touch_pad.c).
 */

#include <stdio.h>
#include <string.h>

#include "touch_pad.h"
#include "ui_input.h"
#include "ui_list.h"
#include "ui_theme.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-48s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static void test_layout(void) {
    printf("layout\n");
    check("two strips and the square fill the width", 2 * TP_STRIP_W + TP_UI_SIZE, TP_PANEL_W);
    check("the square is the panel's height", TP_UI_SIZE, TP_PANEL_H);
    check("the square is the UI at 15/8", TP_UI_SIZE * 8, UI_SCREEN_W * 15);
    check("five keys fill a strip", TP_KEYS * TP_KEY_H, TP_PANEL_H);

    /* Every ten DPAD5 buttons on exactly one key. */
    uint32_t all = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT | BTN_A | BTN_B |
                   BTN_SELECT | BTN_L | BTN_R | BTN_START, seen = 0;
    int keys = 0;
    for (int s = 0; s < 2; s++) {
        for (int r = 0; r < TP_KEYS; r++) {
            const tp_key_t *k = tp_key((tp_side_t)s, r);
            check("one bit per key", __builtin_popcount(k->button), 1);
            check("no button twice", (long)(seen & k->button), 0);
            seen |= k->button;
            keys++;
        }
    }
    check("ten keys", keys, 10);
    check("every DPAD5 button has a key", (long)seen, (long)all);
    check("no key past the last row", tp_key(TP_LEFT, TP_KEYS) == NULL, 1);
    check("no third side", tp_key((tp_side_t)2, 0) == NULL, 1);
}

static void test_hits(void) {
    printf("what a touch is\n");
    /* Every key's centre and its four corners hit that key. */
    for (int s = 0; s < 2; s++) {
        int x0 = s == TP_LEFT ? 0 : TP_RIGHT_X;
        for (int r = 0; r < TP_KEYS; r++) {
            uint32_t b = tp_key((tp_side_t)s, r)->button;
            int y0 = r * TP_KEY_H;
            check("key centre", (long)tp_button_at(x0 + TP_STRIP_W / 2, y0 + TP_KEY_H / 2), (long)b);
            check("key top left", (long)tp_button_at(x0, y0), (long)b);
            check("key bottom right", (long)tp_button_at(x0 + TP_STRIP_W - 1, y0 + TP_KEY_H - 1), (long)b);
        }
    }
    check("the square is not a key", (long)tp_button_at(TP_UI_X, 0), 0);
    check("nor its far edge", (long)tp_button_at(TP_RIGHT_X - 1, TP_PANEL_H - 1), 0);
    check("off the left", (long)tp_button_at(-1, 10), 0);
    check("off the bottom", (long)tp_button_at(10, TP_PANEL_H), 0);
    check("off the right", (long)tp_button_at(TP_PANEL_W, 10), 0);
    check("up is on the left, middle", (long)tp_button_at(20, 120), BTN_UP);
    check("OK is on the right, second", (long)tp_button_at(300, 60), BTN_A);
}

static void test_scale(void) {
    printf("the 15/8 scale\n");
    check("first pixel", tp_dst(0), 0);
    check("the end", tp_dst(UI_SCREEN_W), TP_UI_SIZE);
    int ones = 0, twos = 0;
    for (int s = 0; s < UI_SCREEN_W; s++) {
        int w = tp_dst(s + 1) - tp_dst(s);
        if (w == 1) ones++;
        else if (w == 2) twos++;
        else check("each UI pixel is one or two panel pixels", w, 1);
        for (int p = tp_dst(s); p < tp_dst(s + 1); p++) {
            check("tp_src inverts tp_dst", tp_src(p), s);
        }
    }
    check("16 UI pixels of 8 per 15", ones, 16);
    check("112 doubled", twos, 112);
    check("the last panel pixel shows the last UI pixel", tp_src(TP_UI_SIZE - 1), UI_SCREEN_W - 1);
}

static void test_cal(void) {
    printf("calibration\n");
    tp_cal_t c;
    tp_cal_default(&c);
    int x, y;
    check("default maps", tp_raw_to_panel(&c, 200, 200, &x, &y), 1);
    check("min is the corner x", x, 0);
    check("min is the corner y", y, 0);
    tp_raw_to_panel(&c, 3900, 3900, &x, &y);
    check("max x", x, TP_PANEL_W - 1);
    check("max y", y, TP_PANEL_H - 1);
    tp_raw_to_panel(&c, 2050, 2050, &x, &y);
    check("middle x", x, 160);
    check("middle y", y, 120);
    tp_raw_to_panel(&c, 0, 4095, &x, &y);
    check("below the range clamps", x, 0);
    check("above the range clamps", y, TP_PANEL_H - 1);

    /* A reversed, swapped module: raw Y runs along the long side, backwards. */
    c.swap_xy = true;
    c.x_min = 3900; c.x_max = 200;
    tp_raw_to_panel(&c, 2050, 3900, &x, &y);
    check("swapped: raw Y gives x (reversed)", x, 0);
    check("swapped: raw X gives y", y, 120);
    tp_raw_to_panel(&c, 200, 200, &x, &y);
    check("reversed far end", x, TP_PANEL_W - 1);
    check("reversed below the range", y, 0);

    c.y_min = c.y_max = 1000;
    check("degenerate calibration refused", tp_raw_to_panel(&c, 1, 1, &x, &y), 0);

    /* Round trip: a touch at each key's centre, through the default calibration. */
    tp_cal_default(&c);
    for (int s = 0; s < 2; s++) {
        for (int r = 0; r < TP_KEYS; r++) {
            int px = (s == TP_LEFT ? 0 : TP_RIGHT_X) + TP_STRIP_W / 2, py = r * TP_KEY_H + TP_KEY_H / 2;
            uint16_t rx = (uint16_t)(200 + (3900 - 200) * px / (TP_PANEL_W - 1));
            uint16_t ry = (uint16_t)(200 + (3900 - 200) * py / (TP_PANEL_H - 1));
            tp_raw_to_panel(&c, rx, ry, &x, &y);
            check("a raw touch at a key's centre presses it", (long)tp_button_at(x, y),
                  (long)tp_key((tp_side_t)s, r)->button);
        }
    }
}

static void test_draw(void) {
    printf("strips\n");
    static uint16_t a[TP_STRIP_W * TP_PANEL_H], b[TP_STRIP_W * TP_PANEL_H];
    const ui_theme_t *t = ui_theme();
    tp_draw_strip(a, TP_RIGHT, 0);
    check("the gap is background", a[0], t->bg);
    check("a key face is the band colour", a[(TP_KEY_H / 2) * TP_STRIP_W + 4], t->row_alt);
    int ink = 0;
    for (int i = 0; i < TP_STRIP_W * TP_PANEL_H; i++) {
        ink += a[i] == t->text;
    }
    check("labels are drawn", ink > 50, 1);

    tp_draw_strip(b, TP_RIGHT, BTN_A);
    int changed_in = 0, changed_out = 0;
    for (int r = 0; r < TP_PANEL_H; r++) {
        for (int x = 0; x < TP_STRIP_W; x++) {
            if (a[r * TP_STRIP_W + x] != b[r * TP_STRIP_W + x]) {
                if (r >= TP_KEY_H && r < 2 * TP_KEY_H) changed_in++;
                else changed_out++;
            }
        }
    }
    check("a held key is drawn pressed", changed_in > 100, 1);
    check("and nothing else changes", changed_out, 0);
    check("pressed face is the accent", b[(TP_KEY_H + TP_KEY_H / 2) * TP_STRIP_W + 4], t->accent);
}

int main(void) {
    test_layout();
    test_hits();
    test_scale();
    test_cal();
    test_draw();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
