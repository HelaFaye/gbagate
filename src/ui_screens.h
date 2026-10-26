/*
 * ui_screens.h -- every screen that is not the chip list or a menu.
 *
 * These used to be drawn in pio/src/gate_app.c, straight from the device's
 * globals, so nothing on the host ever drew them: a mode could reach a
 * placeholder, or a line could run off the panel, and no test would notice.
 * Here each screen is built from a plain struct of the state it shows, so a
 * test can build it in any state, render it and look at the pixels.
 *
 * A page is a title, a few lines and a footer. A builder fills one from the
 * state it describes; ui_render_page() draws it. The device only gathers the
 * state and handles the buttons.
 */

#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include <stdbool.h>
#include <stdint.h>

#include "gbl_peer.h"
#include "link_bridge.h"
#include "ui_render.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_PAGE_LINES 8
/*
 * Wider than the 21 glyphs the panel shows, on purpose: a line that does not
 * fit is clipped at render time rather than when it is formatted, so a test
 * can tell an overlong line from a short one (ui_page_line_fits()).
 */
#define UI_PAGE_TEXT  40

typedef struct {
    char     title[UI_PAGE_TEXT];
    uint8_t  count;
    char     line[UI_PAGE_LINES][UI_PAGE_TEXT];
    uint16_t colour[UI_PAGE_LINES];
    char     footer[UI_PAGE_TEXT];
} ui_page_t;

void ui_page_init(ui_page_t *p, const char *title, const char *footer);

/* Append a line; past UI_PAGE_LINES it is dropped. printf-style. */
void ui_page_add(ui_page_t *p, uint16_t colour, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* Append text, wrapped at spaces across as many lines as it needs. */
void ui_page_add_wrapped(ui_page_t *p, uint16_t colour, const char *text);

/* Does line i fit the panel without clipping? */
bool ui_page_line_fits(const ui_page_t *p, uint8_t i);

void ui_render_page(const ui_canvas_t *c, const ui_page_t *p);

/* ---- what each page shows ---------------------------------------------- */

typedef struct {
    uint32_t caps;         /* hw_caps mask */
    bool     linked;
    uint32_t transfers;
    uint32_t chips_sent;
    uint16_t gate_id;
    uint32_t ui_dropped;
} ui_diag_t;

void ui_page_diagnostics(ui_page_t *p, const char *title, const char *footer,
                         const ui_diag_t *d);

/* A mode whose enter() refused: why. */
void ui_page_refused(ui_page_t *p, const char *title, const char *footer,
                     const char *reason);

/* A mode with nothing better to show than its status line. */
void ui_page_status(ui_page_t *p, const char *title, const char *footer,
                    const char *status);

/* Link: USB. b is NULL when the image has no bridge. */
void ui_page_link_usb(ui_page_t *p, const char *title, const char *footer,
                      const link_bridge_t *b);

/* Link: Wireless. mac is the locked peer, or NULL before one is chosen. */
void ui_page_wireless(ui_page_t *p, const char *title, const char *footer,
                      const gbl_peer_t *peer, const uint8_t *mac,
                      const char *air_link);

typedef struct {
    bool     ready;
    uint32_t commands;
    uint32_t aborted;
    uint16_t last;
} ui_ir_t;

void ui_page_infrared(ui_page_t *p, const char *title, const char *footer,
                      const ui_ir_t *ir);

typedef struct {
    bool     reader;       /* a reader board answered */
    bool     advance;      /* a chip is in the Advance slot */
    bool     beast;        /* ... in the Beast slot */
    uint16_t id;           /* decoded wire ID, CG_CHIP_NONE if unreadable */
} ui_scan_t;

void ui_page_chip_scan(ui_page_t *p, const char *title, const char *footer,
                       const ui_scan_t *s);

/* Self Test: the device sends two test chips to rom/tester.gba, which does
 * the checking and shows the verdict on the GBA. */
typedef struct {
    bool     linked;      /* the GBA is clocking us */
    uint32_t transfers;
    uint32_t sent;        /* test chips delivered, of 2 */
} ui_selftest_t;

void ui_page_selftest(ui_page_t *p, const char *title, const char *footer,
                      const ui_selftest_t *t);

/* USB Storage: who owns the volume. */
void ui_page_usb_storage(ui_page_t *p, const char *title, const char *footer,
                         bool exposed);

/* ---- screens that are not pages ---------------------------------------- */

/* Send Program mid-transfer: the program, a status and a bar. */
void ui_render_progress(const ui_canvas_t *c, const char *title,
                        const char *name, const char *status, uint32_t pct,
                        const char *footer);

/* A screen with one thing to say, such as an empty list. */
void ui_render_notice(const ui_canvas_t *c, const char *title, const char *text);

/* Drawn when nothing has started the shell. See gate_app.c. */
void ui_render_no_interface(const ui_canvas_t *c);

#ifdef __cplusplus
}
#endif

#endif /* UI_SCREENS_H */
