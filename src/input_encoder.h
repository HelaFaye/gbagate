/*
 * input_encoder.h -- quadrature encoder with acceleration, plus its button.
 *
 * Sized for the actual problem: 411 chips against 8 visible rows. A plain
 * one-detent-one-row encoder needs 51 full turns to cross the BN6 library,
 * which nobody will do twice. Acceleration scales the step by how fast you are
 * turning, so a slow turn still lands on an exact row and a fast spin covers
 * the list.
 *
 * Portable and free of hardware calls so it can be tested on a host: feed it
 * the two channel levels, the button level and a millisecond timestamp.
 *
 * Debouncing is structural rather than timed. The quadrature state table only
 * accepts legal transitions, so contact bounce produces a +1 immediately
 * followed by a -1 and nets to zero. That is more robust than an RC filter and
 * costs nothing.
 */

#ifndef INPUT_ENCODER_H
#define INPUT_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Most mechanical encoders emit four quadrature edges per physical detent. */
#define ENC_QUARTER_STEPS 4

/* Turn faster than this between detents and acceleration engages. */
#define ENC_ACCEL_WINDOW_MS 150

/* Step multiplier at maximum speed. 20 crosses 411 entries in ~21 detents. */
#define ENC_ACCEL_MAX 20

/* Button hold that counts as a long press. */
#define ENC_LONG_PRESS_MS 500

/* Consecutive agreeing samples before a button level is believed. */
#define ENC_BTN_STABLE 3

typedef struct {
    /* config */
    uint8_t  detent_div;
    uint16_t accel_window_ms;
    uint8_t  accel_max;
    uint16_t long_press_ms;
    uint8_t  btn_stable;
    bool     btn_active_low; /* true for the usual pull-up wiring */

    /* quadrature state */
    uint8_t  prev;      /* last legal (A,B) pair */
    int8_t   quarters;  /* accumulated quarter-steps toward a detent */
    uint32_t last_detent_ms;

    /* button state */
    bool     btn_raw;
    bool     btn_down;
    uint8_t  btn_agree;
    uint32_t btn_down_ms;
    bool     long_fired;

    /* diagnostics -- a high glitch count means bad contacts or long wires */
    uint32_t detents;
    uint32_t glitches;
} encoder_t;

typedef struct {
    int32_t delta;      /* rows to move, sign follows turn direction */
    bool    pressed;    /* button went down this poll */
    bool    released;   /* button came up this poll */
    bool    clicked;    /* released before the long-press threshold */
    bool    long_press; /* held past the threshold; fires once, while held */
} encoder_event_t;

void encoder_init(encoder_t *e, bool btn_active_low);

/*
 * Sample once. Call at 1 kHz or faster; at 200 Hz a fast spin will drop
 * transitions and the glitch counter will climb.
 *
 * a, b       raw channel levels
 * btn_level  raw button level, interpreted per btn_active_low
 * now_ms     monotonic milliseconds
 */
encoder_event_t encoder_poll(encoder_t *e, bool a, bool b, bool btn_level,
                             uint32_t now_ms);

/* Step size acceleration would apply for a given gap between detents. */
uint8_t encoder_accel_step(const encoder_t *e, uint32_t gap_ms);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_ENCODER_H */
