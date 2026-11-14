/*
 * ui_theme.c -- see ui_theme.h
 */

#include "ui_theme.h"

/* RGB565, as COLMOD 0x05 puts it on the wire. Same as ui_render.h's. */
#define C(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static const ui_theme_t k_themes[UI_THEME_COUNT] = {
    [UI_THEME_CLASSIC] = {
        .name = "Classic",
        .bg = C(16, 20, 32),      .row_alt = C(28, 34, 50),
        .header = C(232, 120, 24), .on_header = C(16, 20, 32),
        .text = C(240, 244, 248), .text_dim = C(120, 128, 144),
        .accent = C(255, 208, 64),
        .star_on = C(255, 200, 48), .star_off = C(72, 78, 96),
        .bar_track = C(40, 46, 64), .bar_thumb = C(160, 170, 200),
        .held = C(64, 200, 120),
        .good = C(96, 208, 128), .warn = C(255, 184, 64), .bad = C(255, 96, 88),
    },
    [UI_THEME_NAVI] = {
        .name = "Navi",
        .bg = C(8, 18, 40),       .row_alt = C(18, 34, 64),
        .header = C(56, 152, 248), .on_header = C(8, 18, 40),
        .text = C(224, 240, 255), .text_dim = C(120, 150, 190),
        .accent = C(120, 232, 255),
        .star_on = C(255, 216, 72), .star_off = C(52, 72, 110),
        .bar_track = C(26, 44, 78), .bar_thumb = C(120, 170, 230),
        .held = C(96, 224, 176),
        .good = C(96, 224, 160), .warn = C(255, 200, 80), .bad = C(255, 112, 112),
    },
    [UI_THEME_DMG] = {
        /* The DMG's own four shades, and two between them for the bands. */
        .name = "DMG",
        .bg = C(15, 56, 15),      .row_alt = C(26, 72, 26),
        .header = C(155, 188, 15), .on_header = C(15, 56, 15),
        .text = C(196, 220, 120), .text_dim = C(139, 172, 15),
        .accent = C(220, 236, 140),
        .star_on = C(220, 236, 140), .star_off = C(48, 98, 48),
        .bar_track = C(36, 84, 36), .bar_thumb = C(139, 172, 15),
        .held = C(139, 172, 15),
        .good = C(196, 220, 120), .warn = C(220, 236, 140), .bad = C(240, 248, 200),
    },
    [UI_THEME_CONTRAST] = {
        .name = "Contrast",
        .bg = C(0, 0, 0),         .row_alt = C(36, 36, 36),
        .header = C(255, 224, 0),  .on_header = C(0, 0, 0),
        .text = C(255, 255, 255), .text_dim = C(200, 200, 200),
        .accent = C(255, 224, 0),
        .star_on = C(255, 224, 0), .star_off = C(110, 110, 110),
        .bar_track = C(60, 60, 60), .bar_thumb = C(255, 255, 255),
        .held = C(80, 255, 120),
        .good = C(80, 255, 120), .warn = C(255, 200, 0), .bad = C(255, 96, 96),
    },
    [UI_THEME_DAYLIGHT] = {
        .name = "Daylight",
        .bg = C(244, 242, 236),   .row_alt = C(228, 226, 218),
        .header = C(32, 88, 200),  .on_header = C(255, 255, 255),
        .text = C(20, 24, 32),    .text_dim = C(88, 92, 104),
        .accent = C(176, 80, 0),
        .star_on = C(184, 104, 0), .star_off = C(176, 178, 186),
        .bar_track = C(208, 208, 200), .bar_thumb = C(70, 90, 140),
        .held = C(20, 120, 60),
        .good = C(16, 120, 48), .warn = C(160, 88, 0), .bad = C(184, 32, 32),
    },
};

static ui_theme_id_t s_current = UI_THEME_CLASSIC;

const ui_theme_t *ui_theme(void) { return &k_themes[s_current]; }

void ui_theme_set(ui_theme_id_t id) {
    s_current = (unsigned)id < UI_THEME_COUNT ? id : UI_THEME_CLASSIC;
}

ui_theme_id_t ui_theme_current(void) { return s_current; }

const ui_theme_t *ui_theme_get(ui_theme_id_t id) {
    return &k_themes[(unsigned)id < UI_THEME_COUNT ? id : UI_THEME_CLASSIC];
}

const char *ui_theme_name(ui_theme_id_t id) {
    return (unsigned)id < UI_THEME_COUNT ? k_themes[id].name : "?";
}
