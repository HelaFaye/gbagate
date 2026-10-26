/*
 * chip_dice.c -- roll a random Battle Chip from the chosen game.
 *
 * The example of an applet that asks for a game (needs_game), uses host
 * services (open_db, random), refuses cleanly when it cannot run, and draws
 * a mix of its own art and themed text. Press to roll; turn to step through
 * the chips one at a time from the last roll.
 */

#include <stdio.h>

#include "applet.h"
#include "ui_font.h"

static chipdb_t s_db;
static uint16_t s_slot;        /* the chip on show                         */
static uint32_t s_rolls;
static bool     s_have;        /* a chip has been rolled                   */

/* A slot worth showing: it has a name and a code. Blank table entries --
 * BN6 Gregar's Dark library, for one -- are skipped. */
static bool showable(uint16_t slot) {
    chip_record_t r;
    const char *name = chipdb_name(&s_db, slot);
    return chipdb_get(&s_db, slot, &r) && name && name[0] &&
           chipdb_code_char(r.codes[0]) != 0;
}

static bool dice_enter(const applet_env_t *env) {
    s_rolls = 0;
    s_have  = false;
    /* Refusing is the right answer without a database: the menu says the
     * mode did not start, rather than showing an empty screen. */
    return env->open_db && env->has_game &&
           env->open_db(env->game, &s_db) && chipdb_count(&s_db) > 0;
}

static void roll(const applet_env_t *env) {
    uint16_t n = chipdb_count(&s_db);
    /* A bounded search, so a database with few showable chips cannot hang
     * the UI task. */
    for (int tries = 0; tries < 64; tries++) {
        uint16_t slot = (uint16_t)(env->random() % n);
        if (showable(slot)) {
            s_slot = slot;
            s_have = true;
            s_rolls++;
            return;
        }
    }
}

static void walk(int32_t by) {
    uint16_t n = chipdb_count(&s_db);
    for (uint16_t i = 0; i < n; i++) {
        int32_t next = ((int32_t)s_slot + (by > 0 ? 1 : -1)) % n;
        s_slot = (uint16_t)(next < 0 ? next + n : next);
        if (showable(s_slot)) {
            return;
        }
    }
}

static bool dice_input(const applet_env_t *env, applet_input_t in, int32_t value) {
    switch (in) {
    case APPLET_IN_CONFIRM: roll(env); return true;
    case APPLET_IN_MOVE:
        if (s_have) walk(value);
        return true;
    default:
        return false;
    }
}

static void dice_draw(const applet_env_t *env, const ui_canvas_t *c) {
    const ui_theme_t *t = ui_theme();
    ui_fill(c, 0, 0, UI_SCREEN_W, UI_SCREEN_H, t->bg);
    ui_fill(c, 0, 0, UI_SCREEN_W, UI_HEADER_H, t->header);
    ui_text(c, 3, (UI_HEADER_H - UI_FONT_H) / 2, "Chip Dice", t->on_header);

    if (!s_have) {
        ui_text(c, 4, 40, "press to roll", t->text);
        ui_text_clipped(c, 4, 54, UI_SCREEN_W - 8,
                        chipgate_game_name(env->game), t->text_dim);
    } else {
        chip_record_t r;
        chipdb_get(&s_db, s_slot, &r);
        /* The chip as a card: its element colour, its code large. */
        uint16_t el = ui_element_colour(r.element);
        ui_fill(c, 34, 22, 60, 44, el);
        ui_fill(c, 36, 24, 56, 40, t->bg);
        ui_fill(c, 52, 30, 24, 28, el);
        char code[2] = { chipdb_code_char(r.codes[0]), '\0' };
        /* Bold: the letter drawn four times, a pixel apart. */
        for (int dx = 0; dx < 2; dx++)
            for (int dy = 0; dy < 2; dy++)
                ui_text(c, 62 + dx, 40 + dy, code, COL_ON_ELEMENT);

        const char *name = chipdb_name(&s_db, s_slot);
        int w = (int)ui_font_width(name);
        ui_text_clipped(c, w < UI_SCREEN_W - 4 ? (UI_SCREEN_W - w) / 2 : 2, 72,
                        UI_SCREEN_W - 4, name, t->text);
        char line[24];
        snprintf(line, sizeof line, "#%u  %u MB  x%u", (unsigned)r.lib_number,
                 (unsigned)r.mb, (unsigned)r.gate_uses);
        w = (int)ui_font_width(line);
        ui_text(c, (UI_SCREEN_W - w) / 2, 84, line, t->text_dim);
        snprintf(line, sizeof line, "roll %lu", (unsigned long)s_rolls);
        ui_text(c, 4, 98, line, t->text_dim);
    }

    int fy = UI_SCREEN_H - UI_FOOTER_H;
    ui_fill(c, 0, fy, UI_SCREEN_W, UI_FOOTER_H, t->row_alt);
    ui_fill(c, 0, fy, UI_SCREEN_W, 1, t->header);
    ui_text_clipped(c, 2, fy + (UI_FOOTER_H - UI_FONT_H) / 2, UI_SCREEN_W - 4,
                    "press: roll again", t->text_dim);
}

const applet_t applet_chip_dice = {
    .name        = "Chip Dice",
    .requires    = HW_CAP_DISPLAY,
    .absent_hint = "no display",
    .needs_game  = true,
    .enter       = dice_enter,
    .input       = dice_input,
    .draw        = dice_draw,
};
