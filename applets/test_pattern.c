/*
 * test_pattern.c -- full-screen patterns for checking a panel.
 *
 * The example of an applet that draws its own canvas (draw(), not page()),
 * and a useful one: the ST7735 modules sold as "128x128" differ in where
 * their visible window starts and whether they are RGB or BGR, which is what
 * Settings' offsets and colour order fix. Each pattern answers one question:
 *
 *   bars    are red and blue the right way round?  (if not: colour order)
 *   border  is every edge's line visible?          (if not: offsets)
 *   grid    is it square and evenly spaced?
 *   theme   what does each colour of this theme look like?
 *
 * Turn or press to change pattern.
 */

#include <stdio.h>

#include "applet.h"
#include "ui_font.h"

typedef enum { PAT_BARS = 0, PAT_BORDER, PAT_GRID, PAT_THEME, PAT_COUNT } pattern_t;

static pattern_t s_pattern;

static const char *const k_names[PAT_COUNT] = { "bars", "border", "grid", "theme" };

static bool tp_enter(const applet_env_t *env) {
    (void)env;
    s_pattern = PAT_BARS;
    return true;
}

static void step(int32_t by) {
    int32_t p = ((int32_t)s_pattern + by) % PAT_COUNT;
    s_pattern = (pattern_t)(p < 0 ? p + PAT_COUNT : p);
}

static bool tp_input(const applet_env_t *env, applet_input_t in, int32_t value) {
    (void)env;
    switch (in) {
    case APPLET_IN_MOVE:    step(value > 0 ? 1 : -1); return true;
    case APPLET_IN_CONFIRM: step(1);                  return true;
    default:                return false;
    }
}

/* A label on a plate, so it reads on any pattern behind it. */
static void label(const ui_canvas_t *c, int x, int y, const char *s,
                  uint16_t fg, uint16_t bg) {
    ui_fill(c, x - 1, y - 1, (int)ui_font_width(s) + 2, UI_FONT_H + 2, bg);
    ui_text(c, x, y, s, fg);
}

static void bars(const ui_canvas_t *c) {
    static const struct { uint16_t colour; const char *name; } k[] = {
        { RGB565(255, 255, 255), "W" }, { RGB565(255, 255, 0), "Y" },
        { RGB565(0, 255, 255),   "C" }, { RGB565(0, 255, 0),   "G" },
        { RGB565(255, 0, 255),   "M" }, { RGB565(255, 0, 0),   "R" },
        { RGB565(0, 0, 255),     "B" }, { RGB565(0, 0, 0),     "K" },
    };
    for (int i = 0; i < 8; i++) {
        ui_fill(c, i * 16, 0, 16, UI_SCREEN_H, k[i].colour);
        label(c, i * 16 + 5, 4, k[i].name, RGB565(0, 0, 0), RGB565(255, 255, 255));
    }
}

static void border(const ui_canvas_t *c) {
    const uint16_t white = RGB565(255, 255, 255), black = RGB565(0, 0, 0);
    ui_fill(c, 0, 0, UI_SCREEN_W, UI_SCREEN_H, black);
    /* The outermost row and column on every side. An offset that is one too
     * many hides exactly one of these. */
    ui_fill(c, 0, 0, UI_SCREEN_W, 1, white);
    ui_fill(c, 0, UI_SCREEN_H - 1, UI_SCREEN_W, 1, white);
    ui_fill(c, 0, 0, 1, UI_SCREEN_H, white);
    ui_fill(c, UI_SCREEN_W - 1, 0, 1, UI_SCREEN_H, white);
    /* Corner marks, 8px, so a missing edge says which one. */
    const uint16_t red = RGB565(255, 0, 0);
    ui_fill(c, 2, 2, 8, 8, red);
    ui_fill(c, UI_SCREEN_W - 10, 2, 8, 8, red);
    ui_fill(c, 2, UI_SCREEN_H - 10, 8, 8, red);
    ui_fill(c, UI_SCREEN_W - 10, UI_SCREEN_H - 10, 8, 8, red);
    label(c, 34, 50, "all 4 edges", white, black);
    label(c, 34, 62, "should show", white, black);
}

static void grid(const ui_canvas_t *c) {
    const uint16_t line = RGB565(160, 160, 160);
    ui_fill(c, 0, 0, UI_SCREEN_W, UI_SCREEN_H, RGB565(0, 0, 0));
    for (int v = 0; v < UI_SCREEN_W; v += 16) {
        ui_fill(c, v, 0, 1, UI_SCREEN_H, line);
        ui_fill(c, 0, v, UI_SCREEN_W, 1, line);
    }
    ui_fill(c, UI_SCREEN_W - 1, 0, 1, UI_SCREEN_H, line);
    ui_fill(c, 0, UI_SCREEN_H - 1, UI_SCREEN_W, 1, line);
}

static void theme(const ui_canvas_t *c) {
    const ui_theme_t *t = ui_theme();
    const struct { uint16_t colour; const char *name; } k[] = {
        { t->bg, "bg" }, { t->row_alt, "band" }, { t->header, "header" },
        { t->text, "text" }, { t->text_dim, "dim" }, { t->accent, "accent" },
        { t->good, "good" }, { t->warn, "warn" }, { t->bad, "bad" },
        { t->held, "held" },
    };
    ui_fill(c, 0, 0, UI_SCREEN_W, UI_SCREEN_H, t->bg);
    ui_text(c, 3, 2, t->name, t->text);
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        int y = 12 + (int)i * 11;
        ui_fill(c, 3, y, 20, 9, k[i].colour);
        ui_fill(c, 3, y, 20, 1, t->text_dim);   /* an edge, for bg on bg */
        ui_text(c, 28, y + 1, k[i].name, t->text);
    }
}

static void tp_draw(const applet_env_t *env, const ui_canvas_t *c) {
    (void)env;
    switch (s_pattern) {
    case PAT_BARS:   bars(c);   break;
    case PAT_BORDER: border(c); break;
    case PAT_GRID:   grid(c);   break;
    default:         theme(c);  break;
    }
    char tag[16];
    snprintf(tag, sizeof tag, "%u/%u %s", (unsigned)s_pattern + 1,
             (unsigned)PAT_COUNT, k_names[s_pattern]);
    /* Above the bottom corner marks, so it never hides one. */
    label(c, UI_SCREEN_W - 13 - (int)ui_font_width(tag), UI_SCREEN_H - 20, tag,
          RGB565(255, 255, 255), RGB565(0, 0, 0));
}

const applet_t applet_test_pattern = {
    .name        = "Test Pattern",
    .requires    = HW_CAP_DISPLAY,
    .absent_hint = "no display",
    .enter       = tp_enter,
    .input       = tp_input,
    .draw        = tp_draw,
};
