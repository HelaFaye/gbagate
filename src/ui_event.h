/*
 * ui_event.h -- what the input drivers and the link hand to the UI task.
 *
 * One definition. There used to be three -- in tasks.c, gate_app.c and
 * input_qemu.c -- kept "in step by a compile-time check" that did not exist,
 * and a fourth event type added to one would have been read as another by
 * the others.
 */

#ifndef UI_EVENT_H
#define UI_EVENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_EVENT_ENCODER = 0,   /* move: a = signed steps                       */
    UI_EVENT_CHIP_IN,       /* a chip went into a reader slot               */
    UI_EVENT_CHIP_OUT,      /* back                                         */
    UI_EVENT_LINK_UP,
    UI_EVENT_LINK_DOWN,
    UI_EVENT_CHIP_SENT,     /* confirm: the encoder click, or A             */
    UI_EVENT_USB_ATTACHED,
    UI_EVENT_USB_DETACHED,
    UI_EVENT_TAB,           /* next or previous tab: a = +1 or -1           */
    UI_EVENT_MARK,          /* favourite                                    */
    UI_EVENT_MENU,          /* sort, or a screen's own menu                 */
    UI_EVENT_COUNT
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    int32_t         a;
    uint16_t        b;
} ui_event_t;

#ifdef __cplusplus
}
#endif

#endif /* UI_EVENT_H */
