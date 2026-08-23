/*
 * input_encoder.c -- see input_encoder.h
 */

#include <string.h>

#include "input_encoder.h"

/*
 * Quadrature transition table, indexed by (previous << 2) | current, where each
 * pair is (A << 1) | B. Legal transitions give -1 or +1; the four impossible
 * double-edge transitions give 0 and are counted as glitches, which is what
 * makes contact bounce cancel instead of accumulate.
 */
static const int8_t k_transition[16] = {
     0, +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
     0, -1, +1,  0,
};

void encoder_init(encoder_t *e, bool btn_active_low) {
    memset(e, 0, sizeof *e);
    e->detent_div      = ENC_QUARTER_STEPS;
    e->accel_window_ms = ENC_ACCEL_WINDOW_MS;
    e->accel_max       = ENC_ACCEL_MAX;
    e->long_press_ms   = ENC_LONG_PRESS_MS;
    e->btn_stable      = ENC_BTN_STABLE;
    e->btn_active_low  = btn_active_low;
    e->prev            = 0xFF; /* first sample only establishes a baseline */
}

uint8_t encoder_accel_step(const encoder_t *e, uint32_t gap_ms) {
    if (gap_ms >= e->accel_window_ms || e->accel_max <= 1) {
        return 1;
    }
    /* Linear in how far inside the window the detent landed. Deliberately not
     * exponential: a slow deliberate turn must always move exactly one row, or
     * picking a specific chip becomes a fight. */
    uint32_t span  = e->accel_window_ms;
    uint32_t boost = ((uint32_t)(e->accel_max - 1) * (span - gap_ms)) / span;
    return (uint8_t)(1 + boost);
}

encoder_event_t encoder_poll(encoder_t *e, bool a, bool b, bool btn_level,
                             uint32_t now_ms) {
    encoder_event_t ev;
    memset(&ev, 0, sizeof ev);

    /* ---- quadrature ---- */
    uint8_t cur = (uint8_t)((a ? 2u : 0u) | (b ? 1u : 0u));

    if (e->prev == 0xFF) {
        e->prev = cur;
    } else if (cur != e->prev) {
        int8_t step = k_transition[(e->prev << 2) | cur];
        if (step == 0) {
            e->glitches++;
        } else {
            e->quarters = (int8_t)(e->quarters + step);
        }
        e->prev = cur;
    }

    if (e->quarters >= (int8_t)e->detent_div || e->quarters <= -(int8_t)e->detent_div) {
        int32_t dir = e->quarters > 0 ? 1 : -1;
        e->quarters = 0;

        uint32_t gap = now_ms - e->last_detent_ms;
        if (e->last_detent_ms == 0) {
            gap = e->accel_window_ms; /* first detent never accelerates */
        }
        e->last_detent_ms = now_ms;
        e->detents++;

        ev.delta = dir * (int32_t)encoder_accel_step(e, gap);
    }

    /* ---- button ---- */
    bool level = e->btn_active_low ? !btn_level : btn_level;

    if (level == e->btn_raw) {
        if (e->btn_agree < e->btn_stable) {
            e->btn_agree++;
        }
    } else {
        e->btn_raw   = level;
        e->btn_agree = 1;
    }

    if (e->btn_agree >= e->btn_stable && e->btn_raw != e->btn_down) {
        e->btn_down = e->btn_raw;
        if (e->btn_down) {
            e->btn_down_ms = now_ms;
            e->long_fired  = false;
            ev.pressed     = true;
        } else {
            ev.released = true;
            /* A long press already fired, so releasing after one is not also a
             * click -- otherwise every hold would slot a chip on the way out. */
            ev.clicked = !e->long_fired;
            e->long_fired = false;
        }
    }

    if (e->btn_down && !e->long_fired &&
        (now_ms - e->btn_down_ms) >= e->long_press_ms) {
        e->long_fired  = true;
        ev.long_press  = true;
    }

    return ev;
}
