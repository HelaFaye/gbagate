/*
 * ui_render.h -- draw the chip screen into an RGB565 framebuffer.
 *
 * No SPI, no hardware: the framebuffer is a plain array, so a host test can
 * render a screen and inspect or export the pixels. That is how the layout gets
 * checked without a panel on the desk.
 *
 * Colours follow the homebrew where it matters -- element tinting on the code
 * box, a gold star for favourites -- but the artwork is ours. The homebrew's
 * chip images and font are Capcom's.
 */

#ifndef UI_RENDER_H
#define UI_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "chipdb.h"
#include "ui_chipview.h"
#include "ui_list.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RGB565, which is what COLMOD 0x05 puts on the wire. */
#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define COL_BG        RGB565(16, 20, 32)
#define COL_HEADER    RGB565(232, 120, 24)
#define COL_TEXT      RGB565(240, 244, 248)
#define COL_TEXT_DIM  RGB565(120, 128, 144)
#define COL_ROW_ALT   RGB565(28, 34, 50)
#define COL_CURSOR    RGB565(255, 208, 64)
#define COL_STAR_ON   RGB565(255, 200, 48)
#define COL_STAR_OFF  RGB565(72, 78, 96)
#define COL_BAR_TRACK RGB565(40, 46, 64)
#define COL_BAR_THUMB RGB565(160, 170, 200)
#define COL_HELD      RGB565(64, 200, 120)

/* Element tints for the code box, matching the games' own colour language. */
#define COL_ELEM_NONE RGB565(150, 150, 150)
#define COL_ELEM_FIRE RGB565(232,  72,  40)
#define COL_ELEM_AQUA RGB565( 64, 168, 232)
#define COL_ELEM_ELEC RGB565(240, 208,  48)
#define COL_ELEM_WOOD RGB565( 72, 192,  96)

typedef struct {
    uint16_t *fb;       /* UI_SCREEN_W * UI_SCREEN_H, RGB565 */
    uint16_t  width;
    uint16_t  height;
} ui_canvas_t;

/* Primitives, exposed because the diagnostics and shell screens use them. */
void ui_fill(const ui_canvas_t *c, int x, int y, int w, int h, uint16_t colour);
void ui_text(const ui_canvas_t *c, int x, int y, const char *s, uint16_t colour);
void ui_text_clipped(const ui_canvas_t *c, int x, int y, int max_w,
                     const char *s, uint16_t colour);

uint16_t ui_element_colour(uint8_t element);

/*
 * Draw one list row at the given visible row index. Returns false if the row
 * is blank. Exposed separately from the full screen so the scroll path can
 * render just the strip it is about to reveal.
 */
bool ui_render_row(const ui_canvas_t *c, const chipview_t *v, const chipdb_t *db,
                   uint8_t visible_row, uint16_t item_index, bool selected);

/* Header tabs. Unavailable tabs are drawn dim rather than omitted. */
void ui_render_header(const ui_canvas_t *c, const chipview_t *v);

/* Footer: mode, and the chip most recently sent. */
void ui_render_footer(const ui_canvas_t *c, const chipview_t *v,
                      const char *status);

void ui_render_scrollbar(const ui_canvas_t *c, const ui_list_t *l);

/* Is this list actually backed by this chipview? See the note below. */
bool ui_render_list_matches(const chipview_t *v, const ui_list_t *l);

/*
 * A simple chooser: title, rows of text, one selected. Used for the mode menu,
 * the game list and the self-test screen -- everything that is a list of
 * strings rather than a list of chips.
 *
 * label_at returns NULL past the end, which is how the count is discovered;
 * that keeps this independent of ui_shell so it can render any list of
 * strings.
 */
typedef const char *(*ui_label_fn)(void *ctx, uint8_t index);

void ui_render_menu(const ui_canvas_t *c, const char *title,
                    ui_label_fn label_at, void *ctx,
                    uint8_t count, uint8_t cursor, const char *footer);

/*
 * The whole chip screen.
 *
 * The list must have been filled from this chipview: ui_list_set(l, v->view,
 * v->view_count). The renderer maps a visible row to a chipview position via
 * the list's scroll offset, so a list backed by anything else would draw the
 * wrong chips with no visible symptom. That invariant is checked, and a
 * mismatch paints a blank list rather than plausible nonsense.
 */
void ui_render_chip_screen(const ui_canvas_t *c, const chipview_t *v,
                           const chipdb_t *db, const ui_list_t *l,
                           const char *status);

#ifdef __cplusplus
}
#endif

#endif /* UI_RENDER_H */
