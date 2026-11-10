/*
 * touch_pad.c -- see touch_pad.h.
 */

#include "touch_pad.h"

#include <string.h>

#include "ui_font.h"
#include "ui_input.h"
#include "ui_theme.h"

/*
 * The big line is the button's name as the screens' hints say it ("A send",
 * "B back", "L"/"R" for a page), so a hint can be followed by eye; the small
 * line says what the key does where the name does not.
 */
static const tp_key_t k_keys[2][TP_KEYS] = {
    [TP_LEFT] = {
        { BTN_LEFT,  "<",  "TAB"   },
        { BTN_L,     "L",  "PG UP" },
        { BTN_UP,    "^",  ""      },
        { BTN_DOWN,  "v",  ""      },
        { BTN_R,     "R",  "PG DN" },
    },
    [TP_RIGHT] = {
        { BTN_RIGHT,  ">",  "TAB"  },
        { BTN_A,      "A",  "OK"   },
        { BTN_B,      "B",  "BACK" },
        { BTN_SELECT, "SEL", "FAV" },
        { BTN_START,  "ST", "MENU" },
    },
};

const tp_key_t *tp_key(tp_side_t side, int row) {
    if ((side != TP_LEFT && side != TP_RIGHT) || row < 0 || row >= TP_KEYS) {
        return NULL;
    }
    return &k_keys[side][row];
}

uint32_t tp_button_at(int x, int y) {
    if (x < 0 || y < 0 || x >= TP_PANEL_W || y >= TP_PANEL_H) {
        return 0;
    }
    tp_side_t side;
    if (x < TP_UI_X) {
        side = TP_LEFT;
    } else if (x >= TP_RIGHT_X) {
        side = TP_RIGHT;
    } else {
        return 0;               /* the UI square */
    }
    return k_keys[side][y / TP_KEY_H].button;
}

void tp_cal_default(tp_cal_t *c) {
    c->x_min = 200;
    c->x_max = 3900;
    c->y_min = 200;
    c->y_max = 3900;
    c->swap_xy = false;
}

/* raw from lo..hi (either order) onto 0..span-1, clamped. */
static int map_axis(int raw, int lo, int hi, int span) {
    long off = raw - lo, d = hi - lo;
    if (d < 0) {                /* a reversed axis: the same, mirrored */
        off = -off;
        d = -d;
    }
    if (off < 0) {
        return 0;               /* before the near edge */
    }
    long v = (off * (span - 1) * 2 + d) / (2 * d);   /* to nearest */
    if (v < 0) {
        v = 0;
    } else if (v > span - 1) {
        v = span - 1;
    }
    return (int)v;
}

bool tp_raw_to_panel(const tp_cal_t *c, uint16_t raw_x, uint16_t raw_y, int *x, int *y) {
    if (c->x_min == c->x_max || c->y_min == c->y_max) {
        return false;
    }
    uint16_t rx = c->swap_xy ? raw_y : raw_x;
    uint16_t ry = c->swap_xy ? raw_x : raw_y;
    *x = map_axis(rx, c->x_min, c->x_max, TP_PANEL_W);
    *y = map_axis(ry, c->y_min, c->y_max, TP_PANEL_H);
    return true;
}

/* ---- drawing ----------------------------------------------------------- */

static void fill(uint16_t *buf, int x, int y, int w, int h, uint16_t colour) {
    for (int r = y; r < y + h; r++) {
        for (int col = x; col < x + w; col++) {
            if (r >= 0 && r < TP_PANEL_H && col >= 0 && col < TP_STRIP_W) {
                buf[r * TP_STRIP_W + col] = colour;
            }
        }
    }
}

/* The UI font at `scale`, centred on cx. */
static void text_centred(uint16_t *buf, int cx, int y, const char *s, int scale,
                         uint16_t colour) {
    int n = (int)strlen(s);
    int w = n ? (n * UI_FONT_ADVANCE - 1) * scale : 0;
    int x0 = cx - w / 2;
    for (int i = 0; i < n; i++) {
        const uint8_t *cols = ui_font_glyph(s[i]);
        for (int gx = 0; gx < UI_FONT_W; gx++) {
            for (int gy = 0; gy < UI_FONT_ROWS; gy++) {
                if (cols[gx] & (1u << gy)) {
                    fill(buf, x0 + (i * UI_FONT_ADVANCE + gx) * scale,
                         y + gy * scale, scale, scale, colour);
                }
            }
        }
    }
}

void tp_draw_strip(uint16_t *buf, tp_side_t side, uint32_t held) {
    const ui_theme_t *t = ui_theme();
    fill(buf, 0, 0, TP_STRIP_W, TP_PANEL_H, t->bg);
    for (int row = 0; row < TP_KEYS; row++) {
        const tp_key_t *k = &k_keys[side][row];
        bool down = (held & k->button) != 0;
        int y = row * TP_KEY_H;
        /* A box with a 2px gap all round, so neighbouring keys read apart. */
        uint16_t face = down ? t->accent : t->row_alt;
        uint16_t ink  = down ? t->on_header : t->text;
        fill(buf, 2, y + 2, TP_STRIP_W - 4, TP_KEY_H - 4, face);
        bool has_word = k->word[0] != '\0';
        int glyph_h = UI_FONT_H * 2;
        int block = has_word ? glyph_h + 3 + UI_FONT_H : glyph_h;
        int top = y + (TP_KEY_H - block) / 2;
        text_centred(buf, TP_STRIP_W / 2, top, k->glyph, 2, ink);
        if (has_word) {
            text_centred(buf, TP_STRIP_W / 2, top + glyph_h + 3, k->word, 1,
                         down ? t->on_header : t->text_dim);
        }
    }
}
