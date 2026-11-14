/*
 * touch_pad.h -- the touch carrier's 320x240 panel: where the UI goes, where
 * the touch keys go, and what a touch means.
 *
 * The UI is drawn at 128x128 on every board. On the touch carrier
 * (doc/CARRIER.md) the panel is 320x240 in landscape: the UI is scaled by
 * 15/8 into the 240x240 square in the middle, and the 40-pixel strips either
 * side hold five touch keys each. The keys are the GBA-style buttons the
 * d-pad boards have (ui_input.h, INPUT_DPAD5), so every screen, its hints and
 * its long presses work unchanged: a touch is a button held for as long as
 * the finger stays on it.
 *
 *    left strip            right strip
 *    <   TAB    (LEFT)     >   TAB   (RIGHT)
 *    L   PG UP  (L)        A   OK    (A)
 *    ^          (UP)       B   BACK  (B)
 *    v          (DOWN)     SEL FAV   (SELECT)
 *    R   PG DN  (R)        ST  MENU  (START)
 *
 * A touch on the UI square itself is not a key (yet): 0.
 *
 * Portable and host-tested (test/test_touch_pad.c); the SPI and pins are in
 * port/esp32s3/lcd_ili9341_esp32s3.c and touch_xpt2046_esp32s3.c.
 */

#ifndef TOUCH_PAD_H
#define TOUCH_PAD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TP_PANEL_W   320
#define TP_PANEL_H   240
#define TP_STRIP_W   40                       /* each side                   */
#define TP_UI_X      TP_STRIP_W               /* the UI square's left edge   */
#define TP_UI_SIZE   240                      /* 128 x 15/8                  */
#define TP_RIGHT_X   (TP_UI_X + TP_UI_SIZE)   /* 280: the right strip        */
#define TP_KEYS      5                        /* per strip                   */
#define TP_KEY_H     (TP_PANEL_H / TP_KEYS)   /* 48                          */

typedef enum { TP_LEFT = 0, TP_RIGHT = 1 } tp_side_t;

typedef struct {
    uint32_t    button;   /* a ui_btn_t                        */
    const char *glyph;    /* the big line: "<", "OK", ...      */
    const char *word;     /* the small line under it, or ""    */
} tp_key_t;

/* The key at row 0..TP_KEYS-1 of a strip. */
const tp_key_t *tp_key(tp_side_t side, int row);

/* The button under a panel point, or 0 for the UI square or off the panel. */
uint32_t tp_button_at(int x, int y);

/* ---- the 15/8 scale ---------------------------------------------------- */

/* The UI pixel a panel pixel (0..TP_UI_SIZE-1) of the square shows. */
static inline int tp_src(int panel) { return panel * 8 / 15; }

/* The first panel pixel of UI pixel src (0..128): src 128 gives TP_UI_SIZE.
 * UI pixel src covers [tp_dst(src), tp_dst(src + 1)), one or two pixels. */
static inline int tp_dst(int src) { return (src * 15 + 7) / 8; }

/* ---- calibration ------------------------------------------------------- */

/*
 * From the XPT2046's raw 12-bit plate readings to panel pixels. Which plate
 * runs along which edge, and which way, is the module's: the defaults are an
 * X plate along the 320 side and 200..3900 on both, which is what most 2.8"
 * modules read and what qemu_esp32's xpt2046 gives by default.
 */
typedef struct {
    uint16_t x_min, x_max;    /* raw X at panel x 0 and x 319 (after swap)  */
    uint16_t y_min, y_max;    /* raw Y at panel y 0 and y 239               */
    bool     swap_xy;         /* the X plate runs along the short side      */
} tp_cal_t;

void tp_cal_default(tp_cal_t *c);

/* A raw reading to a panel point, clamped to the panel. False if the
 * calibration is degenerate (min == max). */
bool tp_raw_to_panel(const tp_cal_t *c, uint16_t raw_x, uint16_t raw_y, int *x, int *y);

/* Pressure from the Z1/Z2 plates is not used: the XPT2046's PENIRQ says
 * touched, and a reading below this on both plates is a lifted finger
 * caught mid-conversion, so it is dropped. */
#define TP_RAW_FLOOR 64

/* ---- the strips -------------------------------------------------------- */

/*
 * Draw one strip into a TP_STRIP_W x TP_PANEL_H RGB565 buffer, row major:
 * each key a box with its glyph at twice the UI font and its word under it,
 * in the theme's colours; the keys in `held` (ui_btn_t bits) drawn pressed.
 */
void tp_draw_strip(uint16_t *buf, tp_side_t side, uint32_t held);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_PAD_H */
