/*
 * input_events.c -- see input_events.h
 */

#include "input_events.h"

bool input_event_for(ctrl_action_t a, int32_t value, ui_event_t *out) {
    ui_event_t e = { .type = UI_EVENT_COUNT, .a = 0, .b = 0 };
    switch (a) {
    case CTRL_PREV:      e.type = UI_EVENT_ENCODER; e.a = -1;    break;
    case CTRL_NEXT:      e.type = UI_EVENT_ENCODER; e.a = +1;    break;
    /* Paging carries its own signed step (UI_IN_PAGE rows). */
    case CTRL_PAGE_PREV: e.type = UI_EVENT_ENCODER; e.a = value; break;
    case CTRL_PAGE_NEXT: e.type = UI_EVENT_ENCODER; e.a = value; break;
    case CTRL_CONFIRM:   e.type = UI_EVENT_CHIP_SENT;            break;
    case CTRL_CANCEL:    e.type = UI_EVENT_CHIP_OUT;             break;
    case CTRL_TAB_PREV:  e.type = UI_EVENT_TAB;     e.a = -1;    break;
    case CTRL_TAB_NEXT:  e.type = UI_EVENT_TAB;     e.a = +1;    break;
    case CTRL_MARK:      e.type = UI_EVENT_MARK;                 break;
    case CTRL_MENU:      e.type = UI_EVENT_MENU;                 break;
    default:             return false;
    }
    /* The encoder's turns arrive as PREV/NEXT with their accelerated step. */
    if ((a == CTRL_PREV || a == CTRL_NEXT) && value != 0) {
        e.a = value;
    }
    if (out) {
        *out = e;
    }
    return true;
}

size_t input_drain(controls_t *c, uint32_t now_ms, input_post_fn post) {
    size_t n = 0;
    /* Bounded: a driver that kept producing intents must not pin its task. */
    for (int i = 0; i < 32; i++) {
        int32_t v = 0;
        ctrl_action_t a = controls_next(c, now_ms, &v);
        if (a == CTRL_NONE) {
            break;
        }
        ui_event_t e;
        if (input_event_for(a, v, &e) && post && post(&e)) {
            n++;
        }
    }
    return n;
}
