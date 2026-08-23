/*
 * st7735_panel.h -- panel geometry, window mapping and hardware vertical
 * scroll for ST7735-family displays. No SPI calls: this computes the register
 * values, so the arithmetic can be tested on a host and the driver stays thin.
 *
 * The controller's frame memory is 132 x 162 regardless of the glass attached
 * to it, because the same die serves several panel sizes. A 128x128 module
 * therefore looks at a 128x128 window inside that memory, and a 128x160 module
 * at a 128x160 one. Which window is why "not cropped" does not imply a zero
 * offset -- the 1.44" 128x128 boards almost always want colstart 2, rowstart 3.
 *
 * What the spare memory is and is not good for
 * --------------------------------------------
 * Spare columns (132 - 128 = 4) are useless. No glass sits behind them, so
 * nothing written there is ever visible, and reading it back is not a practical
 * scratchpad: RAMRD runs far slower than writes, and most breakout modules do
 * not wire MISO at all.
 *
 * Spare rows are a different story, because the controller can scroll. VSCRDEF
 * splits the 162 rows into a top fixed area, a scrolling area and a bottom
 * fixed area, and VSCRSADD chooses which memory row appears at the top of the
 * scrolling one. Point the scrolling area at the chip list and moving one row
 * costs a single 12px row write plus one register write, instead of repainting
 * all eight rows:
 *
 *     full list repaint   128 x 96 x 2 = 24576 B  ~12 ms at 20 MHz
 *     hardware scroll     128 x 12 x 2 =  3072 B  ~1.5 ms
 *
 * Eight times cheaper, which is the difference between an encoder that feels
 * direct and one that smears. Note this works by reusing the row that just
 * scrolled off the top, so it needs no off-panel rows at all -- the 34 spare
 * rows on a 128x128 panel are a bonus for staging a row ahead, not a
 * requirement. A 128x160 panel has only 2 spare rows and still scrolls fine.
 */

#ifndef ST7735_PANEL_H
#define ST7735_PANEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Controller frame memory, fixed regardless of the glass. */
#define ST7735_GRAM_W 132
#define ST7735_GRAM_H 162

typedef enum {
    ST7735_PANEL_128X128 = 0, /* default: 1.44" green tab */
    ST7735_PANEL_128X160      /* 1.8" red tab             */
} st7735_panel_t;

/* Typical offsets. Both are overridable, because module vendors vary and the
 * only reliable way to settle it is a calibration pattern. */
#define ST7735_128X128_COLSTART 2
#define ST7735_128X128_ROWSTART 3
#define ST7735_128X160_COLSTART 2
#define ST7735_128X160_ROWSTART 1

typedef struct {
    st7735_panel_t panel;
    uint8_t width;
    uint8_t height;
    uint8_t colstart;
    uint8_t rowstart;

    /* scroll state */
    uint8_t  scroll_top;    /* first panel row of the scrolling area   */
    uint8_t  scroll_rows;   /* height of the scrolling area, in pixels */
    uint16_t tfa, vsa, bfa; /* as programmed into VSCRDEF              */
    uint16_t scroll_offset; /* pixels scrolled, 0 .. vsa-1             */
    bool     scroll_ready;
} st7735_panel_cfg_t;

/* Configure for a panel type using the typical offsets. */
void st7735_panel_init(st7735_panel_cfg_t *p, st7735_panel_t panel);

/* Override the offsets after a calibration pass. */
void st7735_panel_set_offset(st7735_panel_cfg_t *p, uint8_t colstart, uint8_t rowstart);

/*
 * Map a logical rectangle to CASET/RASET arguments. Returns false if the
 * rectangle falls outside the panel, which is worth catching: an off-by-one
 * here writes into invisible memory and simply shows nothing.
 */
bool st7735_window(const st7735_panel_cfg_t *p, uint8_t x, uint8_t y,
                   uint8_t w, uint8_t h,
                   uint16_t *x0, uint16_t *x1, uint16_t *y0, uint16_t *y1);

/*
 * Define the scrolling area over panel rows [top, top + rows). The fixed areas
 * absorb everything else, including any off-panel rows, so that
 * tfa + vsa + bfa == 162 as the controller requires.
 */
bool st7735_scroll_define(st7735_panel_cfg_t *p, uint8_t top, uint8_t rows);

/* VSCRDEF arguments. Call after st7735_scroll_define. */
void st7735_scroll_args(const st7735_panel_cfg_t *p,
                        uint16_t *tfa, uint16_t *vsa, uint16_t *bfa);

/*
 * Scroll by delta pixels, positive meaning content moves up (the list advances).
 * Returns the VSCRSADD value to write.
 */
uint16_t st7735_scroll_by(st7735_panel_cfg_t *p, int32_t delta);

/* Absolute scroll position, in pixels from the top of the scrolling area. */
uint16_t st7735_scroll_set(st7735_panel_cfg_t *p, uint16_t offset);

/* Current VSCRSADD value. */
uint16_t st7735_scroll_addr(const st7735_panel_cfg_t *p);

/*
 * After scrolling by `rows` pixels, this is the memory row where the newly
 * exposed strip must be written -- the strip that just wrapped around. Feed it
 * to st7735_window_raw() to build the address window.
 */
uint16_t st7735_scroll_fill_row(const st7735_panel_cfg_t *p, int32_t delta);

/*
 * Window for a strip already expressed in controller memory rows, for use with
 * the value above. Handles the wrap inside the scrolling area by reporting how
 * many rows fit before the wrap; the caller writes the remainder at the other
 * end.
 */
void st7735_window_raw(const st7735_panel_cfg_t *p, uint16_t mem_row,
                       uint8_t rows, uint16_t *y0, uint16_t *y1,
                       uint8_t *rows_before_wrap);

/* How many memory rows exist that no glass sits behind. */
uint8_t st7735_offpanel_rows(const st7735_panel_cfg_t *p);

#ifdef __cplusplus
}
#endif

#endif /* ST7735_PANEL_H */
