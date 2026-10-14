/*
 * ui_settings.c -- see ui_settings.h
 */

#include <stdio.h>
#include <string.h>

#include "chipgate.h"
#include "ui_input.h"
#include "ui_settings.h"

static bool s_dirty;
static bool s_display_dirty;

void uiset_init(ui_settings_t *v, settings_t *s) {
    memset(v, 0, sizeof *v);
    v->s = s;
    s_dirty = false;
    s_display_dirty = false;
}

uint8_t uiset_count(void) { return SETFIELD_COUNT; }

static const char *field_name(setfield_t f) {
    switch (f) {
    case SETFIELD_COLSTART: return "col offset";
    case SETFIELD_ROWSTART: return "row offset";
    case SETFIELD_COLOUR:   return "colour";
    case SETFIELD_GAME:     return "game";
    case SETFIELD_ACCEL:    return "accel";
    case SETFIELD_WRAP:     return "wrap";
    case SETFIELD_INPUT:    return "input";
    default:                return "?";
    }
}

static void field_value(const settings_t *s, setfield_t f, char *out, size_t cap) {
    switch (f) {
    case SETFIELD_COLSTART: snprintf(out, cap, "%u", s->colstart); break;
    case SETFIELD_ROWSTART: snprintf(out, cap, "%u", s->rowstart); break;
    case SETFIELD_COLOUR:   snprintf(out, cap, "%s", s->bgr ? "BGR" : "RGB"); break;
    case SETFIELD_GAME:
        snprintf(out, cap, "%s", chipgate_game_name((cg_game_t)s->game));
        break;
    case SETFIELD_ACCEL:    snprintf(out, cap, "%u", s->accel); break;
    case SETFIELD_WRAP:     snprintf(out, cap, "%s", s->wrap ? "on" : "off"); break;
    case SETFIELD_INPUT:
        snprintf(out, cap, "%s", input_mode_name((input_mode_t)s->input_mode));
        break;
    default:                snprintf(out, cap, "?"); break;
    }
}

const char *uiset_label(void *ctx, uint8_t index) {
    ui_settings_t *v = ctx;
    if (index >= SETFIELD_COUNT) {
        return NULL;
    }
    char val[24];
    field_value(v->s, (setfield_t)index, val, sizeof val);

    /*
     * Brackets around the value being edited. With one encoder doing both jobs
     * there is otherwise nothing on screen that says which mode you are in,
     * and turning the knob does something different depending on an invisible
     * state.
     */
    bool active = v->editing && index == v->cursor;
    snprintf(v->scratch, sizeof v->scratch, active ? "%-11s[%s]" : "%-11s %s",
             field_name((setfield_t)index), val);
    return v->scratch;
}

/* Clamp rather than wrap: an offset that rolls from 8 to 0 while you are
 * nudging it looks like the knob slipped. Toggles wrap, because with two
 * values there is nowhere else to go. */
static void adjust(settings_t *s, setfield_t f, int32_t d) {
    int32_t v;
    switch (f) {
    case SETFIELD_COLSTART:
        v = (int32_t)s->colstart + d;
        s->colstart = (uint8_t)(v < 0 ? 0 : (v > 8 ? 8 : v));
        s_display_dirty = true;
        break;
    case SETFIELD_ROWSTART:
        v = (int32_t)s->rowstart + d;
        s->rowstart = (uint8_t)(v < 0 ? 0 : (v > 8 ? 8 : v));
        s_display_dirty = true;
        break;
    case SETFIELD_COLOUR:
        s->bgr = !s->bgr;
        s_display_dirty = true;
        break;
    case SETFIELD_GAME:
        v = ((int32_t)s->game + d) % CG_GAME_COUNT;
        if (v < 0) {
            v += CG_GAME_COUNT;
        }
        s->game = (uint8_t)v;
        break;
    case SETFIELD_ACCEL:
        v = (int32_t)s->accel + d;
        s->accel = (uint8_t)(v < 1 ? 1 : (v > 40 ? 40 : v));
        break;
    case SETFIELD_WRAP:
        s->wrap = !s->wrap;
        break;
    case SETFIELD_INPUT: {
        /*
         * Wraps rather than clamps: with three values there is nowhere else to
         * go, and someone who has picked the wrong one needs to get back.
         */
        int32_t m = ((int32_t)s->input_mode + d) % INPUT_MODE_COUNT;
        if (m < 0) {
            m += INPUT_MODE_COUNT;
        }
        s->input_mode = (uint8_t)m;
        break;
    }
    default:
        return;
    }
    s_dirty = true;
}

void uiset_move(ui_settings_t *v, int32_t delta) {
    if (delta == 0) {
        return;
    }
    if (!v->editing) {
        int32_t pos = ((int32_t)v->cursor + delta) % SETFIELD_COUNT;
        if (pos < 0) {
            pos += SETFIELD_COUNT;
        }
        v->cursor = (uint8_t)pos;
        return;
    }
    /*
     * One detent at a time while editing, whatever the encoder reports. The
     * acceleration that helps across 400 chips makes an offset unsettable:
     * a flick moves it the whole range and back.
     */
    adjust(v->s, (setfield_t)v->cursor, delta > 0 ? 1 : -1);
}

bool uiset_toggle_edit(ui_settings_t *v) {
    v->editing = !v->editing;
    return v->editing;
}

bool uiset_dirty(const ui_settings_t *v) { (void)v; return s_dirty; }
void uiset_clear_dirty(ui_settings_t *v) { (void)v; s_dirty = false; }

bool uiset_display_affected(const ui_settings_t *v) {
    (void)v;
    return s_display_dirty;
}

const char *uiset_footer(const ui_settings_t *v) {
    return v->editing ? "turn to change" : "enter to edit";
}
