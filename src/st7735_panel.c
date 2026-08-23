/*
 * st7735_panel.c -- see st7735_panel.h
 */

#include <string.h>

#include "st7735_panel.h"

void st7735_panel_init(st7735_panel_cfg_t *p, st7735_panel_t panel) {
    memset(p, 0, sizeof *p);
    p->panel = panel;
    p->width = 128;
    if (panel == ST7735_PANEL_128X160) {
        p->height   = 160;
        p->colstart = ST7735_128X160_COLSTART;
        p->rowstart = ST7735_128X160_ROWSTART;
    } else {
        p->height   = 128;
        p->colstart = ST7735_128X128_COLSTART;
        p->rowstart = ST7735_128X128_ROWSTART;
    }
}

void st7735_panel_set_offset(st7735_panel_cfg_t *p, uint8_t colstart, uint8_t rowstart) {
    p->colstart = colstart;
    p->rowstart = rowstart;
}

uint8_t st7735_offpanel_rows(const st7735_panel_cfg_t *p) {
    return (uint8_t)(ST7735_GRAM_H - p->height);
}

bool st7735_window(const st7735_panel_cfg_t *p, uint8_t x, uint8_t y,
                   uint8_t w, uint8_t h,
                   uint16_t *x0, uint16_t *x1, uint16_t *y0, uint16_t *y1) {
    if (w == 0 || h == 0) {
        return false;
    }
    if ((uint16_t)x + w > p->width || (uint16_t)y + h > p->height) {
        return false; /* would land in memory no glass sits behind */
    }
    if (x0) *x0 = (uint16_t)(x + p->colstart);
    if (x1) *x1 = (uint16_t)(x + p->colstart + w - 1);
    if (y0) *y0 = (uint16_t)(y + p->rowstart);
    if (y1) *y1 = (uint16_t)(y + p->rowstart + h - 1);
    return true;
}

bool st7735_scroll_define(st7735_panel_cfg_t *p, uint8_t top, uint8_t rows) {
    if (rows == 0 || (uint16_t)top + rows > p->height) {
        return false;
    }

    /* The controller requires the three areas to account for all 162 memory
     * rows. Everything above the scrolling area is fixed; everything below it,
     * including the rows with no glass behind them, is fixed too. */
    uint16_t tfa = (uint16_t)(p->rowstart + top);
    uint16_t vsa = rows;
    if (tfa + vsa > ST7735_GRAM_H) {
        return false;
    }
    uint16_t bfa = (uint16_t)(ST7735_GRAM_H - tfa - vsa);

    p->scroll_top    = top;
    p->scroll_rows   = rows;
    p->tfa           = tfa;
    p->vsa           = vsa;
    p->bfa           = bfa;
    p->scroll_offset = 0;
    p->scroll_ready  = true;
    return true;
}

void st7735_scroll_args(const st7735_panel_cfg_t *p,
                        uint16_t *tfa, uint16_t *vsa, uint16_t *bfa) {
    if (tfa) *tfa = p->tfa;
    if (vsa) *vsa = p->vsa;
    if (bfa) *bfa = p->bfa;
}

uint16_t st7735_scroll_addr(const st7735_panel_cfg_t *p) {
    if (!p->scroll_ready) {
        return 0;
    }
    /* VSCRSADD is an absolute memory row, so bias by the top fixed area. */
    return (uint16_t)(p->tfa + p->scroll_offset);
}

uint16_t st7735_scroll_set(st7735_panel_cfg_t *p, uint16_t offset) {
    if (!p->scroll_ready || p->vsa == 0) {
        return 0;
    }
    p->scroll_offset = (uint16_t)(offset % p->vsa);
    return st7735_scroll_addr(p);
}

uint16_t st7735_scroll_by(st7735_panel_cfg_t *p, int32_t delta) {
    if (!p->scroll_ready || p->vsa == 0) {
        return 0;
    }
    int32_t vsa = (int32_t)p->vsa;
    int32_t pos = ((int32_t)p->scroll_offset + delta) % vsa;
    if (pos < 0) {
        pos += vsa;
    }
    p->scroll_offset = (uint16_t)pos;
    return st7735_scroll_addr(p);
}

uint16_t st7735_scroll_fill_row(const st7735_panel_cfg_t *p, int32_t delta) {
    if (!p->scroll_ready || p->vsa == 0) {
        return 0;
    }
    int32_t vsa = (int32_t)p->vsa;
    int32_t n = delta % vsa;
    if (n < 0) {
        n += vsa;
    }

    /*
     * With VSCRSADD = S the scroll area shows memory rows S, S+1, ... wrapping
     * inside [tfa, tfa + vsa). Scrolling up by n advances S by n, so the n rows
     * that just left the top -- memory rows S_old .. S_old + n - 1 -- are
     * exactly the ones now exposed at the bottom. Repaint those.
     */
    int32_t start;
    if (delta >= 0) {
        start = ((int32_t)p->scroll_offset - n + vsa) % vsa;
    } else {
        /* Scrolling down exposes rows at the top, starting at the new S. */
        start = (int32_t)p->scroll_offset;
    }
    return (uint16_t)(p->tfa + start);
}

void st7735_window_raw(const st7735_panel_cfg_t *p, uint16_t mem_row,
                       uint8_t rows, uint16_t *y0, uint16_t *y1,
                       uint8_t *rows_before_wrap) {
    uint16_t area_end = (uint16_t)(p->tfa + p->vsa);
    uint16_t avail = (mem_row < area_end) ? (uint16_t)(area_end - mem_row) : 0;
    uint8_t fit = (rows <= avail) ? rows : (uint8_t)avail;

    if (y0) *y0 = mem_row;
    if (y1) *y1 = (uint16_t)(mem_row + (fit ? fit - 1 : 0));
    if (rows_before_wrap) *rows_before_wrap = fit;
}
