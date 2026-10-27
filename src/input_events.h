/*
 * input_events.h -- from a control intent to the event the UI task reads.
 *
 * Every input driver -- the encoder and switches on hardware, the keyboard
 * under QEMU -- binds the same control set and turns its intents into events
 * here, so the hardware and the emulator cannot disagree about what a press
 * means. They did: QEMU delivered the favourite mark and nothing else did,
 * and nobody delivered tabs or sort.
 */

#ifndef INPUT_EVENTS_H
#define INPUT_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controls.h"
#include "ui_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Everything a screen can ask for. A driver binds this once; a screen's own
 * binding then only decides which buttons need a long press. */
#define INPUT_WANTED (CTRL_BIT(CTRL_PREV) | CTRL_BIT(CTRL_NEXT) | \
                      CTRL_BIT(CTRL_PAGE_PREV) | CTRL_BIT(CTRL_PAGE_NEXT) | \
                      CTRL_BIT(CTRL_TAB_PREV) | CTRL_BIT(CTRL_TAB_NEXT) | \
                      CTRL_BIT(CTRL_CONFIRM) | CTRL_BIT(CTRL_CANCEL) | \
                      CTRL_BIT(CTRL_MARK) | CTRL_BIT(CTRL_MENU))

/* The event for an intent. False for CTRL_NONE and anything unmapped. */
bool input_event_for(ctrl_action_t a, int32_t value, ui_event_t *out);

typedef bool (*input_post_fn)(const ui_event_t *e);

/* Turn every pending intent into an event and post it. One poll can yield a
 * press and a repeat, so this drains. Returns how many were posted. */
size_t input_drain(controls_t *c, uint32_t now_ms, input_post_fn post);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_EVENTS_H */
