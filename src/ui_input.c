/*
 * ui_input.c -- see ui_input.h
 */

#include <string.h>

#include "ui_input.h"

void ui_input_init(ui_input_t *in, input_mode_t mode,
                   ui_in_read_fn read, void *ctx) {
    memset(in, 0, sizeof *in);
    in->mode      = mode;
    in->read      = read;
    in->ctx       = ctx;
    in->long_mask = ui_input_default_long(mode);
}

const char *input_mode_name(input_mode_t m) {
    switch (m) {
    case INPUT_ENCODER: return "encoder";
    case INPUT_DPAD2:   return "5-way + 2";
    case INPUT_DPAD4:   return "5-way + 4";
    case INPUT_DPAD5:   return "5-way + 5";
    default:            return "?";
    }
}

uint8_t input_mode_switches(input_mode_t m) {
    switch (m) {
    case INPUT_ENCODER: return 1;   /* the encoder's own button */
    case INPUT_DPAD2:   return 7;   /* five directions plus two */
    case INPUT_DPAD4:   return 9;
    case INPUT_DPAD5:   return 10;
    default:            return 0;
    }
}

/*
 * Which buttons exist in this mode. A four-switch build reads L and R; a
 * two-switch build must ignore those bits rather than trust whatever a
 * floating pin reads, which on a board with pull-ups is "pressed".
 */
static uint32_t mode_mask(input_mode_t m) {
    uint32_t base = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT | BTN_A;
    switch (m) {
    case INPUT_ENCODER: return BTN_A;
    case INPUT_DPAD2:   return base | BTN_B | BTN_SELECT;
    case INPUT_DPAD4:   return base | BTN_B | BTN_SELECT | BTN_L | BTN_R;
    case INPUT_DPAD5:   return base | BTN_B | BTN_SELECT | BTN_L | BTN_R |
                               BTN_START;
    default:            return 0;
    }
}

/* Only these repeat. A repeating Select would toggle a favourite dozens of
 * times from one held press. */
static bool repeats(uint32_t b) {
    return b == BTN_UP || b == BTN_DOWN || b == BTN_L || b == BTN_R;
}

/* Only A has a long press, and only where there is no Start button to put
 * sort on. Giving B a long press too would make "back" feel unreliable. */
uint32_t ui_input_default_long(input_mode_t m) {
    switch (m) {
    case INPUT_ENCODER:
        /* No B at all, so the long press is the only way back. */
        return BTN_A;
    case INPUT_DPAD2:
    case INPUT_DPAD4:
        /* No Start, so A holds for sort. */
        return BTN_A;
    case INPUT_DPAD5:
        /* Start owns sort, so nothing needs a hold by default and A can fire
         * on the edge. A screen that wants long-A can still ask for it. */
        return 0;
    default:
        return 0;
    }
}

void ui_input_want_long(ui_input_t *in, uint32_t buttons) {
    if (in->long_mask == buttons) {
        return;
    }
    in->long_mask = buttons;
    /*
     * Forget anything mid-press. A button held across the change would
     * otherwise be judged by the new rule against a press that began under the
     * old one -- releasing it could emit a short press the caller never armed,
     * or swallow one it did.
     */
    memset(in->held_since_ms, 0, sizeof in->held_since_ms);
    memset(in->long_fired, 0, sizeof in->long_fired);
}

static bool has_long(const ui_input_t *in, uint32_t b) {
    return (in->long_mask & b) != 0;
}

static ui_in_event_t map_press(input_mode_t mode, uint32_t b) {
    ui_in_event_t e = { UI_IN_NONE, 0, b, false };

    switch (b) {
    case BTN_UP:    e.kind = UI_IN_MOVE; e.value = -1; break;
    case BTN_DOWN:  e.kind = UI_IN_MOVE; e.value = +1; break;
    case BTN_LEFT:  e.kind = UI_IN_TAB;  e.value = -1; break;
    case BTN_RIGHT: e.kind = UI_IN_TAB;  e.value = +1; break;
    case BTN_L:     e.kind = UI_IN_MOVE; e.value = -UI_IN_PAGE; break;
    case BTN_R:     e.kind = UI_IN_MOVE; e.value = +UI_IN_PAGE; break;
    case BTN_B:     e.kind = UI_IN_BACK; break;
    case BTN_SELECT: e.kind = UI_IN_FAVOURITE; break;
    case BTN_A:     e.kind = UI_IN_SELECT; break;
    case BTN_START: e.kind = UI_IN_SORT;   break;
    default: break;
    }
    (void)mode;
    return e;
}

ui_in_event_t ui_input_poll(ui_input_t *in, uint32_t now_ms) {
    ui_in_event_t none = { UI_IN_NONE, 0, 0, false };

    if (!in->read) {
        return none;
    }

    uint32_t raw = in->read(in->ctx) & mode_mask(in->mode);

    /*
     * Debounce by requiring the reading to hold still. A counter of stable
     * samples would depend on how often this is called; a time window does
     * not, and this is polled from the UI task at whatever rate it happens to
     * run.
     */
    if (raw != in->candidate) {
        in->candidate    = raw;
        in->candidate_ms = now_ms;
    } else if (raw != in->stable &&
               (uint32_t)(now_ms - in->candidate_ms) >= UI_IN_DEBOUNCE_MS) {
        in->stable = raw;
    }

    uint32_t state = in->stable;

    for (int i = 0; i < 10; i++) {
        uint32_t b = 1u << i;
        if (!(mode_mask(in->mode) & b)) {
            continue;
        }
        bool down = (state & b) != 0;

        if (down && in->held_since_ms[i] == 0) {
            /* Edge: pressed. */
            in->held_since_ms[i]  = now_ms ? now_ms : 1;
            in->next_repeat_ms[i] = now_ms + UI_IN_REPEAT_1ST_MS;
            in->long_fired[i]     = false;

            /*
             * A button with a long press reports nothing yet -- the press is
             * only known to be short once it is released. Firing on the edge
             * and again on the hold would send both meanings.
             */
            if (!has_long(in, b)) {
                return map_press(in->mode, b);
            }
            continue;
        }

        if (!down && in->held_since_ms[i] != 0) {
            /* Edge: released. A short press on a long-press button reports
             * its ordinary meaning now. */
            bool was_long = in->long_fired[i];
            in->held_since_ms[i] = 0;
            in->long_fired[i]    = false;
            if (has_long(in, b) && !was_long) {
                return map_press(in->mode, b);
            }
            continue;
        }

        if (!down) {
            continue;
        }

        /* Held. */
        if (has_long(in, b) && !in->long_fired[i] &&
            (uint32_t)(now_ms - in->held_since_ms[i]) >= UI_IN_LONG_MS) {
            in->long_fired[i] = true;
            ui_in_event_t e = { UI_IN_NONE, 0, b, true };
            /* On the encoder the long press is the only way back; with a
             * five-way there is a B button, so long-A means sort instead.
             * controls.c ignores this and resolves by button, which is what
             * lets two different actions share a held control. */
            e.kind = (in->mode == INPUT_ENCODER) ? UI_IN_BACK : UI_IN_SORT;
            return e;
        }

        if (repeats(b) && (int32_t)(now_ms - in->next_repeat_ms[i]) >= 0) {
            in->next_repeat_ms[i] = now_ms + UI_IN_REPEAT_MS;
            return map_press(in->mode, b);
        }
    }

    in->last_ms = now_ms;
    return none;
}

void ui_input_encoder_delta(ui_input_t *in, int32_t detents) {
    (void)in;
    (void)detents;
    /* The encoder's rotation is handled by input_encoder.c, which already
     * accumulates and accelerates; this file only owns its button. Kept so
     * the two paths have one obvious entry point each. */
}
