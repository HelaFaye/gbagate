/*
 * ui_input.h -- one input model, two kinds of hardware.
 *
 * A rotary encoder and a five-way switch want different code but should
 * produce the same interface. This is that layer: it debounces whatever is
 * fitted, applies auto-repeat and long-press, and emits the same events either
 * way, so nothing above it knows which board it is running on.
 *
 * Why the five-way mapping looks the way it does
 * ----------------------------------------------
 * It is the homebrew's own control scheme, which players already know:
 *
 *   up / down       choose a chip
 *   left / right    change library tab
 *   A (the click)   send, or place in Move mode
 *   B               back
 *   Select          toggle favourite
 *   L / R           page the list, when four switches are fitted
 *
 * Start, and who decides about long presses
 * -----------------------------------------
 * Start opens the sort menu on a real GBA. With two or four switches there is
 * no button left for it, so a long press on A stands in.
 *
 * A button that has a long press cannot report its short meaning until it is
 * released: until the button comes up, the press might still become a hold. So
 * a long press always costs latency on the short press, and it is worth being
 * careful about who pays it.
 *
 * That cost used to be decided by the *mode*: A fired on release with two or
 * four switches and on the edge with five. That made A's timing a property of
 * which board was fitted, which is the wrong thing for it to depend on. Code
 * above this layer could not rely on long-A existing, and a screen that wanted
 * it would work on one board and not another.
 *
 * So it is declared per context instead, with ui_input_want_long(). A screen
 * that uses a long press asks for it and pays the latency; a screen that does
 * not gets the edge response. The default is derived from the mode, so a
 * caller that never asks keeps the sensible behaviour -- but any screen, on
 * any board, can now have a long press if it needs one.
 *
 * The encoder keeps its own arrangement -- turn to move, click to select, hold
 * to go back -- because with one control there is nothing else available.
 *
 * Reading the switches is injected, so debounce, repeat and the mapping are
 * all testable on a host.
 */

#ifndef UI_INPUT_H
#define UI_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INPUT_ENCODER = 0,  /* rotary encoder with a button          */
    INPUT_DPAD2,        /* five-way switch plus B and Select     */
    INPUT_DPAD4,        /* five-way switch plus B, Select, L, R  */
    INPUT_DPAD5,        /* ...and Start, which completes the GBA set */
    INPUT_MODE_COUNT
} input_mode_t;

/* Physical buttons, as bits in the mask the reader returns. A five-way
 * switch's click is A; the encoder's button is also A. */
typedef enum {
    BTN_UP     = 1u << 0,
    BTN_DOWN   = 1u << 1,
    BTN_LEFT   = 1u << 2,
    BTN_RIGHT  = 1u << 3,
    BTN_A      = 1u << 4,
    BTN_B      = 1u << 5,
    BTN_SELECT = 1u << 6,
    BTN_L      = 1u << 7,
    BTN_R      = 1u << 8,
    BTN_START  = 1u << 9
} ui_btn_t;

/* What the UI is told. Deliberately about intent, not about which switch. */
typedef enum {
    UI_IN_NONE = 0,
    UI_IN_MOVE,        /* value: -1 or +1, or +-page for L/R */
    UI_IN_SELECT,      /* A, or the encoder button           */
    UI_IN_BACK,        /* B, or a long press on the encoder  */
    UI_IN_TAB,         /* value: -1 or +1                    */
    UI_IN_FAVOURITE,   /* Select                             */
    UI_IN_SORT         /* long press on A                    */
} ui_in_kind_t;

typedef struct {
    ui_in_kind_t kind;
    int32_t      value;
    /*
     * Which control produced this, and whether it was held. The kind alone is
     * not enough once two actions can be bound to the same button: a hold on A
     * might be the sort menu or something the loaded content asked for, and
     * only the binding knows which. The kind stays for callers that use this
     * layer directly; controls.c resolves by button.
     */
    uint32_t     button;
    bool         held;
} ui_in_event_t;

/*
 * Timing. Repeat exists because holding down through 400 chips one detent at a
 * time is not usable; the first repeat waits longer than the rest so a single
 * deliberate press does not become two.
 */
#define UI_IN_DEBOUNCE_MS   12
#define UI_IN_REPEAT_1ST_MS 400
#define UI_IN_REPEAT_MS     90
#define UI_IN_LONG_MS       600
#define UI_IN_PAGE          8    /* rows L/R jump, matching the list height */

/* Read the raw switch mask; a set bit means pressed. */
typedef uint32_t (*ui_in_read_fn)(void *ctx);

typedef struct {
    input_mode_t  mode;
    ui_in_read_fn read;
    void         *ctx;

    uint32_t stable;        /* debounced state          */
    uint32_t candidate;     /* what we are waiting on   */
    uint32_t candidate_ms;

    uint32_t held_since_ms[10];
    uint32_t next_repeat_ms[10];
    bool     long_fired[10];

    uint32_t long_mask;     /* buttons that currently have a long press */
    uint32_t last_ms;
} ui_input_t;

void ui_input_init(ui_input_t *in, input_mode_t mode,
                   ui_in_read_fn read, void *ctx);

/*
 * Declare which buttons need long-press detection right now. Buttons in the
 * mask report their short meaning on release and a long press after
 * UI_IN_LONG_MS; buttons outside it fire on the edge.
 *
 * Call it when a screen is entered. The mask is not cumulative -- it replaces
 * whatever was set -- so a screen cannot leave latency behind on a button the
 * next screen wanted to be immediate.
 */
void ui_input_want_long(ui_input_t *in, uint32_t buttons);

/* The mask ui_input_init() chose from the mode, for a caller that wants to
 * add to the default rather than replace it. */
uint32_t ui_input_default_long(input_mode_t mode);

/*
 * Poll. Returns the next event, or UI_IN_NONE. Call repeatedly until it
 * returns NONE: one poll can produce a press and a repeat.
 */
ui_in_event_t ui_input_poll(ui_input_t *in, uint32_t now_ms);

/* For the encoder path, which produces movement rather than button presses. */
void ui_input_encoder_delta(ui_input_t *in, int32_t detents);

const char *input_mode_name(input_mode_t m);

/* How many switches a mode needs, for the diagnostics screen. */
uint8_t input_mode_switches(input_mode_t m);

#ifdef __cplusplus
}
#endif

#endif /* UI_INPUT_H */
