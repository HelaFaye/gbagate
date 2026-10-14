/*
 * controls.c -- see controls.h
 */

#include <stdio.h>
#include <string.h>

#include "controls.h"

/*
 * How each action is reached, per input mode.
 *
 * `btn` is the switch; `hold` means it must be held. A zero button means the
 * action cannot be performed on that hardware -- which is a real answer, not a
 * gap: an encoder has one button and cannot do everything.
 */
typedef struct { uint32_t btn; bool hold; } binding_t;

static binding_t bind_for(input_mode_t m, ctrl_action_t a) {
    binding_t none = { 0, false };

    switch (m) {
    case INPUT_ENCODER:
        /*
         * One control. Rotation is movement, the button confirms, holding it
         * goes back. Everything else is out of reach, and the screen is told
         * so rather than being given a button that does two things.
         */
        switch (a) {
        case CTRL_PREV:    return (binding_t){ BTN_UP, false };   /* turn */
        case CTRL_NEXT:    return (binding_t){ BTN_DOWN, false };
        case CTRL_CONFIRM: return (binding_t){ BTN_A, false };
        case CTRL_CANCEL:  return (binding_t){ BTN_A, true };
        /* One button, already doing two jobs. Content asking for a hold here
         * is asking for the only way back, so it does not get it. */
        case CTRL_ALT:     return (binding_t){ BTN_A, true };
        default:           return none;
        }

    case INPUT_DPAD2:
        switch (a) {
        case CTRL_PREV:     return (binding_t){ BTN_UP, false };
        case CTRL_NEXT:     return (binding_t){ BTN_DOWN, false };
        case CTRL_TAB_PREV: return (binding_t){ BTN_LEFT, false };
        case CTRL_TAB_NEXT: return (binding_t){ BTN_RIGHT, false };
        case CTRL_CONFIRM:  return (binding_t){ BTN_A, false };
        case CTRL_CANCEL:   return (binding_t){ BTN_B, false };
        case CTRL_MARK:     return (binding_t){ BTN_SELECT, false };
        /* No Start, so sort borrows a hold on A. */
        case CTRL_MENU:     return (binding_t){ BTN_A, true };
        /* Content's own hold. The same control sort would use -- which is why
         * the two conflict on this hardware and the conflict is reported. */
        case CTRL_ALT:      return (binding_t){ BTN_A, true };
        default:            return none;   /* no L/R: no paging */
        }

    case INPUT_DPAD4:
        switch (a) {
        case CTRL_PAGE_PREV: return (binding_t){ BTN_L, false };
        case CTRL_PAGE_NEXT: return (binding_t){ BTN_R, false };
        default:             return bind_for(INPUT_DPAD2, a);
        }

    case INPUT_DPAD5:
        switch (a) {
        /*
         * Start owns sort, which frees A's hold entirely -- so content can
         * have it, and confirm still fires on the edge because CTRL_ALT's hold
         * is on A while CONFIRM's press is the same button.
         *
         * That is the one place this gets subtle: a hold and a press on the
         * same button are not independent. Asking for CTRL_ALT here does cost
         * CONFIRM its edge response, and that is the honest trade rather than
         * a hidden one.
         */
        case CTRL_MENU: return (binding_t){ BTN_START, false };
        default:        return bind_for(INPUT_DPAD4, a);
        }

    default:
        return none;
    }
}

void controls_bind(controls_t *c, ui_input_t *in, uint32_t wanted) {
    c->in     = in;
    c->wanted = wanted;
    c->mode   = in ? in->mode : INPUT_ENCODER;
    c->hint[0] = '\0';

    /*
     * Only ask for the long presses this screen actually needs. A screen that
     * does not use the menu gets an immediate confirm even on a board where
     * the menu would have required a hold -- which is the whole point of
     * ui_input_want_long() being per context.
     */
    /*
     * Walk in enum order and let the first claimant keep a control. Two
     * actions can want the same one -- sort and a content hold both want
     * hold-A on a two-switch board -- and something has to lose. Ordering the
     * enum is what decides it, which keeps the rule visible in one place
     * rather than spread across the bindings.
     */
    uint32_t claimed_press = 0, claimed_hold = 0;
    uint32_t longs = 0;
    c->blocked = 0;

    for (int a = 1; a < CTRL_ACTION_COUNT; a++) {
        if (!(wanted & CTRL_BIT(a))) {
            continue;
        }
        binding_t b = bind_for(c->mode, (ctrl_action_t)a);
        if (!b.btn) {
            continue;   /* unreachable on this hardware; not a conflict */
        }
        uint32_t *claimed = b.hold ? &claimed_hold : &claimed_press;
        if (*claimed & b.btn) {
            c->blocked |= CTRL_BIT(a);
            continue;
        }
        *claimed |= b.btn;
        if (b.hold) {
            longs |= b.btn;
        }
    }
    if (in) {
        ui_input_want_long(in, longs);
    }
}

uint32_t controls_conflicts(const controls_t *c) { return c->blocked; }

bool controls_available(const controls_t *c, ctrl_action_t a) {
    if (c->blocked & CTRL_BIT(a)) {
        return false;   /* reachable in principle, taken by something earlier */
    }
    return bind_for(c->mode, a).btn != 0;
}

const char *controls_label(const controls_t *c, ctrl_action_t a) {
    if (c->blocked & CTRL_BIT(a)) {
        return NULL;
    }
    binding_t b = bind_for(c->mode, a);
    if (!b.btn) {
        return NULL;
    }
    if (c->mode == INPUT_ENCODER) {
        /* Naming a d-pad direction on a board that has none would be a lie;
         * the encoder turns and clicks. */
        switch (b.btn) {
        case BTN_UP:   return "turn";
        case BTN_DOWN: return "turn";
        case BTN_A:    return b.hold ? "hold click" : "click";
        default:       return NULL;
        }
    }
    switch (b.btn) {
    case BTN_UP:     return "up";
    case BTN_DOWN:   return "down";
    case BTN_LEFT:   return "left";
    case BTN_RIGHT:  return "right";
    case BTN_A:      return b.hold ? "hold A" : "A";
    case BTN_B:      return "B";
    case BTN_SELECT: return "Select";
    case BTN_L:      return "L";
    case BTN_R:      return "R";
    case BTN_START:  return "Start";
    default:         return NULL;
    }
}

const char *ctrl_action_name(ctrl_action_t a) {
    switch (a) {
    case CTRL_PREV:      return "up";
    case CTRL_NEXT:      return "down";
    case CTRL_PAGE_PREV: return "page";
    case CTRL_PAGE_NEXT: return "page";
    case CTRL_TAB_PREV:  return "tab";
    case CTRL_TAB_NEXT:  return "tab";
    case CTRL_CONFIRM:   return "send";
    case CTRL_CANCEL:    return "back";
    case CTRL_MARK:      return "fav";
    case CTRL_MENU:      return "sort";
    case CTRL_ALT:       return "alt";
    default:             return "";
    }
}

/*
 * The hint. Only the actions worth naming: movement is discoverable by trying
 * it, and a footer listing "up down left right" crowds out the ones that are
 * not obvious.
 */
const char *controls_hint(controls_t *c) {
    static const ctrl_action_t shown[] = {
        CTRL_CONFIRM, CTRL_CANCEL, CTRL_MARK, CTRL_MENU
    };

    size_t used = 0;
    c->hint[0] = '\0';

    for (unsigned i = 0; i < sizeof shown / sizeof shown[0]; i++) {
        ctrl_action_t a = shown[i];
        if (!(c->wanted & CTRL_BIT(a))) {
            continue;
        }
        const char *label = controls_label(c, a);
        if (!label) {
            continue;   /* unreachable here: say nothing rather than lie */
        }
        const char *what = ctrl_action_name(a);

        size_t need = strlen(label) + 1 + strlen(what) + (used ? 2 : 0);
        if (used + need + 1 > sizeof c->hint) {
            break;      /* the display clips anyway; stop on a whole entry */
        }
        int n = snprintf(c->hint + used, sizeof c->hint - used,
                         "%s%s %s", used ? "  " : "", label, what);
        if (n < 0) {
            break;
        }
        used += (size_t)n;
    }
    return c->hint;
}

ctrl_action_t controls_next(controls_t *c, uint32_t now_ms, int32_t *value) {
    if (value) {
        *value = 0;
    }
    if (!c->in) {
        return CTRL_NONE;
    }

    ui_in_event_t e = ui_input_poll(c->in, now_ms);
    if (e.kind == UI_IN_NONE) {
        return CTRL_NONE;
    }
    int32_t v = e.value;

    /*
     * Resolve by control, not by kind. Two wanted actions can share a button
     * -- sort and a content hold both want hold-A where there is no Start --
     * and the kind cannot tell them apart. The binding table can, and it is
     * the same table that decided who got the control in the first place.
     */
    ctrl_action_t a = CTRL_NONE;
    for (int i = 1; i < CTRL_ACTION_COUNT; i++) {
        if (!(c->wanted & CTRL_BIT(i)) || (c->blocked & CTRL_BIT(i))) {
            continue;
        }
        binding_t b = bind_for(c->mode, (ctrl_action_t)i);
        if (b.btn == e.button && b.hold == e.held) {
            a = (ctrl_action_t)i;
            break;
        }
    }
    if (a != CTRL_NONE) {
        if (value) {
            *value = v;
        }
        return a;
    }

    /* Movement has no single button -- an encoder turns -- so fall back to
     * the kind for those. */
    switch (e.kind) {
    case UI_IN_MOVE:
        /* ui_input reports a page jump as a large step; the distinction is
         * worth keeping so a screen can scroll rather than move a cursor. */
        if (v <= -UI_IN_PAGE)      { a = CTRL_PAGE_PREV; }
        else if (v >= UI_IN_PAGE)  { a = CTRL_PAGE_NEXT; }
        else if (v < 0)            { a = CTRL_PREV; }
        else if (v > 0)            { a = CTRL_NEXT; }
        break;
    case UI_IN_TAB:       a = (v < 0) ? CTRL_TAB_PREV : CTRL_TAB_NEXT; break;
    case UI_IN_SELECT:    a = CTRL_CONFIRM; break;
    case UI_IN_BACK:      a = CTRL_CANCEL;  break;
    case UI_IN_FAVOURITE: a = CTRL_MARK;    break;
    case UI_IN_SORT:      a = CTRL_MENU;    break;
    default:              return CTRL_NONE;
    }

    /*
     * Drop what this screen did not ask for. Without it a stray press on a
     * button the screen has no use for reaches it anyway, and every screen
     * ends up filtering by hand.
     */
    if (!(c->wanted & CTRL_BIT(a))) {
        return CTRL_NONE;
    }
    if (value) {
        *value = v;
    }
    return a;
}
