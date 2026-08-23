/*
 * ui_list.h -- scrolling list model sized for a 128x128 ST7735.
 *
 * The display is the constraint that shapes everything here. At 128x128 with a
 * 5x7 font on a 6px advance you get 21 columns, which fits the longest chip
 * name in the tables (11 chars: "ProtoMn[EX]", "GyroMan[SP]"). Vertically:
 *
 *     header  14px   library tab + link status
 *     list    96px   8 rows x 12px
 *     footer  18px   mode + last chip sent
 *
 * Per row across 128px: star 8 + name 66 + code 8 + MB 20 + element 10 = 112,
 * leaving 16 for a scrollbar.
 *
 * Eight visible rows against 411 chips is the real problem, not pixel count.
 * Three things make it tractable, two of which the original homebrew already
 * proved: library tabs cap the worst list at 232, favourites and folders are
 * the actual play path, and a rotary encoder with acceleration beats a d-pad
 * badly. This model supports all three and stays free of any display calls so
 * it can be tested on a host.
 */

#ifndef UI_LIST_MODEL_H
#define UI_LIST_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Layout, in pixels. Kept here so the renderer and the model cannot disagree. */
#define UI_SCREEN_W      128
#define UI_SCREEN_H      128
#define UI_HEADER_H      14
#define UI_FOOTER_H      18
#define UI_ROW_H         12
#define UI_ROWS          8
#define UI_LIST_Y        UI_HEADER_H
#define UI_LIST_H        (UI_ROW_H * UI_ROWS)

/*
 * Row columns, following the homebrew's own arrangement:
 *
 *     > [icon] Name        [code] 3 *  |bar
 *
 * The cursor and a small element/library glyph lead; the favourite star is on
 * the right where the homebrew puts it. Uses -- the 1-or-3 Chip Gate Use Limit
 * -- gets a column because the homebrew treats it as headline information and
 * it is the field that governs play. MB moves to the detail view: it only
 * matters when building a folder.
 *
 * The code box is tinted by element rather than spending a separate column,
 * which is again what the homebrew does.
 */
#define UI_COL_CURSOR_X  0
#define UI_COL_CURSOR_W  6
#define UI_COL_ICON_X    6
#define UI_COL_ICON_W   10
#define UI_COL_NAME_X   18
#define UI_COL_NAME_W   66   /* 11 chars at a 6px advance, the longest name */
#define UI_COL_CODE_X   86
#define UI_COL_CODE_W   10
#define UI_COL_USES_X   98
#define UI_COL_USES_W    8
#define UI_COL_STAR_X  108
#define UI_COL_STAR_W    8
#define UI_SCROLLBAR_X 120
#define UI_SCROLLBAR_W   4

/* A full frame is 128*128*2 = 32 KB. At 20 MHz SPI that is ~13 ms, so redraw
 * dirty rows rather than the screen -- and never block the link task on it. */
#define UI_FRAME_BYTES   (UI_SCREEN_W * UI_SCREEN_H * 2)

typedef struct {
    const uint16_t *items;  /* filtered view: indices into the chip database */
    uint16_t        count;
    uint16_t        cursor; /* selected item                                */
    uint16_t        top;    /* item shown on the first visible row          */
    uint8_t         rows;   /* visible rows, normally UI_ROWS               */
    bool            wrap;   /* wrap at the ends: right for a rotary encoder */
    bool            dirty;  /* set on any change the renderer must act on   */
} ui_list_t;

void ui_list_init(ui_list_t *l, uint8_t rows, bool wrap);

/* Point the list at a filtered view. Keeps the cursor if it still fits. */
void ui_list_set(ui_list_t *l, const uint16_t *items, uint16_t count);

/* Move the cursor by delta, scrolling the window to follow. */
void ui_list_move(ui_list_t *l, int32_t delta);

/* Whole screens. */
void ui_list_page(ui_list_t *l, int32_t pages);

/* Jump to an absolute position, e.g. from an alphabet index. */
void ui_list_goto(ui_list_t *l, uint16_t position);

/* Item shown on a given visible row. False if that row is blank. */
bool ui_list_row(const ui_list_t *l, uint8_t row, uint16_t *item);

/* Currently selected item. False if the list is empty. */
bool ui_list_selected(const ui_list_t *l, uint16_t *item);

/* Which visible row the cursor sits on. */
uint8_t ui_list_cursor_row(const ui_list_t *l);

/*
 * Scrollbar geometry in pixels within the list area, so the renderer does not
 * repeat the arithmetic. Returns false when everything fits and no bar is
 * wanted.
 */
bool ui_list_scrollbar(const ui_list_t *l, uint8_t *y, uint8_t *h);

#ifdef __cplusplus
}
#endif

#endif /* UI_LIST_MODEL_H */
