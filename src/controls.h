/*
 * controls.h -- what a screen wants to do, not which button does it.
 *
 * ui_input.c turns switches into events. This turns events into *intent*, and
 * owns the part that kept going wrong: which physical control performs an
 * action depends on the hardware fitted, and so does whether that control
 * needs a long press.
 *
 * A screen declares the actions it uses. The handler then:
 *
 *   - picks the control for each action, per input mode;
 *   - works out which buttons need long-press detection and tells ui_input,
 *     so no screen sets that mask by hand;
 *   - reports which actions are unreachable on this hardware, so a screen can
 *     hide a function rather than offer one that cannot be invoked;
 *   - writes the footer hint, from the same table.
 *
 * That last one is the reason this exists. Footers were literals -- one of
 * them read "back: backspace", naming a key on the QEMU viewer's keyboard,
 * which is wrong on every real board and on two of the four input modes. A
 * hint derived from the same table that dispatches the button cannot drift
 * from it.
 */

#ifndef CONTROLS_H
#define CONTROLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What screens ask for. Named for intent: a screen wants to confirm, not to
 * "press A", because on an encoder there is no A.
 */
typedef enum {
    CTRL_NONE = 0,
    CTRL_PREV,       /* previous item          */
    CTRL_NEXT,       /* next item              */
    CTRL_PAGE_PREV,  /* jump back a screenful  */
    CTRL_PAGE_NEXT,
    CTRL_TAB_PREV,   /* previous category      */
    CTRL_TAB_NEXT,
    CTRL_CONFIRM,    /* send, select, enter    */
    CTRL_CANCEL,     /* back                   */
    CTRL_MARK,       /* toggle favourite       */
    CTRL_MENU,       /* sort, options          */
    /*
     * A secondary action belonging to whatever is running -- a mode, or an
     * applet loaded into one. Deliberately last: when two actions want the
     * same control, the earlier one in this list keeps it, so content can ask
     * for a hold without being able to take "back" or "sort" away from the
     * shell.
     */
    CTRL_ALT,
    CTRL_ACTION_COUNT
} ctrl_action_t;

#define CTRL_BIT(a) (1u << (a))

typedef struct {
    ui_input_t   *in;
    uint32_t      wanted;     /* bitmask of ctrl_action_t the screen uses */
    input_mode_t  mode;
    uint32_t      blocked;    /* wanted, but its control was already taken */
    char          hint[40];
} controls_t;

/*
 * Bind a screen's control set. Replaces any previous set, and updates
 * ui_input's long-press mask to match -- so a screen never has to reason about
 * long presses, only about what it does.
 */
void controls_bind(controls_t *c, ui_input_t *in, uint32_t wanted_actions);

/* Next intent, or CTRL_NONE. `value` carries the step for movement actions. */
ctrl_action_t controls_next(controls_t *c, uint32_t now_ms, int32_t *value);

/*
 * Can this action be performed on the fitted hardware? An encoder has one
 * button and cannot reach everything; a screen should hide what it cannot
 * offer rather than showing a function with no way to invoke it.
 */
bool controls_available(const controls_t *c, ctrl_action_t a);

/*
 * Actions that were asked for and could not be given a control, because
 * something earlier in the list already holds it. Not an error -- on a
 * two-switch board sort and a content hold both want hold-A, and one of them
 * has to lose. The point is that it is reported rather than silently decided.
 */
uint32_t controls_conflicts(const controls_t *c);

/* The name of the control that performs an action here: "A", "Start",
 * "hold A", "click". NULL if unreachable. */
const char *controls_label(const controls_t *c, ctrl_action_t a);

/* A footer hint built from the bound set, e.g. "A send  B back  Start sort". */
const char *controls_hint(controls_t *c);

const char *ctrl_action_name(ctrl_action_t a);

#ifdef __cplusplus
}
#endif

#endif /* CONTROLS_H */
