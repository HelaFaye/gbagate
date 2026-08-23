/*
 * test_panel.c -- ST7735 geometry and hardware scroll arithmetic.
 *
 *   cc -std=c11 -Wall -Wextra -o test_panel test_panel.c st7735_panel.c
 */

#include <stdio.h>

#include "st7735_panel.h"
#include "ui_list.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-42s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static void test_geometry(void) {
    printf("panel geometry\n");

    st7735_panel_cfg_t p;
    st7735_panel_init(&p, ST7735_PANEL_128X128);
    check("default is 128x128", p.height, 128);
    check("colstart", p.colstart, 2);
    check("rowstart", p.rowstart, 3);
    check("34 rows with no glass", st7735_offpanel_rows(&p), 34);

    st7735_panel_cfg_t q;
    st7735_panel_init(&q, ST7735_PANEL_128X160);
    check("160 tall", q.height, 160);
    check("rowstart", q.rowstart, 1);
    check("only 2 spare rows", st7735_offpanel_rows(&q), 2);
}

static void test_window(void) {
    printf("address windows\n");

    st7735_panel_cfg_t p;
    st7735_panel_init(&p, ST7735_PANEL_128X128);

    uint16_t x0, x1, y0, y1;
    check("full screen accepted", st7735_window(&p, 0, 0, 128, 128, &x0, &x1, &y0, &y1), 1);
    check("x0 offset applied", x0, 2);
    check("x1", x1, 129);
    check("y0 offset applied", y0, 3);
    check("y1", y1, 130);

    /* The list area from ui_list, which is what actually gets redrawn. */
    check("list area", st7735_window(&p, 0, UI_LIST_Y, 128, UI_LIST_H, &x0, &x1, &y0, &y1), 1);
    check("list y0", y0, 3 + UI_HEADER_H);
    check("list y1", y1, 3 + UI_HEADER_H + UI_LIST_H - 1);

    /* Off-panel writes must be refused, not silently swallowed: they land in
     * memory with no glass behind it and simply show nothing. */
    check("too wide refused", st7735_window(&p, 120, 0, 16, 8, &x0, &x1, &y0, &y1), 0);
    check("too tall refused", st7735_window(&p, 0, 120, 8, 16, &x0, &x1, &y0, &y1), 0);
    check("zero size refused", st7735_window(&p, 0, 0, 0, 8, &x0, &x1, &y0, &y1), 0);

    /* A calibration pass may move the origin. */
    st7735_panel_set_offset(&p, 2, 1);
    st7735_window(&p, 0, 0, 8, 8, &x0, &x1, &y0, &y1);
    check("recalibrated rowstart", y0, 1);
}

static void test_scroll_setup(void) {
    printf("scroll region\n");

    st7735_panel_cfg_t p;
    st7735_panel_init(&p, ST7735_PANEL_128X128);
    check("define over the list", st7735_scroll_define(&p, UI_LIST_Y, UI_LIST_H), 1);

    uint16_t tfa, vsa, bfa;
    st7735_scroll_args(&p, &tfa, &vsa, &bfa);
    printf("  tfa=%u vsa=%u bfa=%u\n", tfa, vsa, bfa);

    /* The controller requires all 162 memory rows to be accounted for. */
    check("areas cover all memory", tfa + vsa + bfa, ST7735_GRAM_H);
    check("tfa is rowstart + header", tfa, 3 + UI_HEADER_H);
    check("vsa is the list", vsa, UI_LIST_H);
    check("bfa absorbs footer + spare", bfa, ST7735_GRAM_H - (3 + UI_HEADER_H) - UI_LIST_H);

    /* Same layout on the taller panel must also balance. */
    st7735_panel_cfg_t q;
    st7735_panel_init(&q, ST7735_PANEL_128X160);
    check("defines on 128x160", st7735_scroll_define(&q, UI_LIST_Y, UI_LIST_H), 1);
    st7735_scroll_args(&q, &tfa, &vsa, &bfa);
    check("still covers memory", tfa + vsa + bfa, ST7735_GRAM_H);

    /* Nonsense must be refused. */
    check("past the bottom refused", st7735_scroll_define(&p, 100, 64), 0);
    check("zero height refused", st7735_scroll_define(&p, 14, 0), 0);
}

static void test_scroll_motion(void) {
    printf("scroll motion\n");

    st7735_panel_cfg_t p;
    st7735_panel_init(&p, ST7735_PANEL_128X128);
    st7735_scroll_define(&p, UI_LIST_Y, UI_LIST_H);
    uint16_t base = p.tfa;

    check("starts at the top", st7735_scroll_addr(&p), base);

    /* One list row is 12px. */
    check("one row down", st7735_scroll_by(&p, UI_ROW_H), base + 12);
    check("two rows", st7735_scroll_by(&p, UI_ROW_H), base + 24);

    /* Wrap at the end of the scroll area, not past it. */
    st7735_scroll_set(&p, UI_LIST_H - 4);
    check("near the end", st7735_scroll_addr(&p), base + UI_LIST_H - 4);
    check("wraps", st7735_scroll_by(&p, 8), base + 4);

    /* And backwards. */
    st7735_scroll_set(&p, 4);
    check("wraps backwards", st7735_scroll_by(&p, -8), base + UI_LIST_H - 4);

    /* Large deltas must fold, not overflow. */
    st7735_scroll_set(&p, 0);
    check("huge delta folds", st7735_scroll_by(&p, 100000) < base + UI_LIST_H, 1);
    st7735_scroll_set(&p, 0);
    check("huge negative folds", st7735_scroll_by(&p, -100000) < base + UI_LIST_H, 1);
}

static void test_fill_row(void) {
    printf("newly exposed strip\n");

    st7735_panel_cfg_t p;
    st7735_panel_init(&p, ST7735_PANEL_128X128);
    st7735_scroll_define(&p, UI_LIST_Y, UI_LIST_H);
    uint16_t base = p.tfa;

    /* Scrolling down by one row: the row that left the top is the one now
     * exposed at the bottom, so it is the one to repaint. */
    st7735_scroll_by(&p, UI_ROW_H);
    check("fill row after +1", st7735_scroll_fill_row(&p, UI_ROW_H), base + 0);

    st7735_scroll_by(&p, UI_ROW_H);
    check("fill row after +2", st7735_scroll_fill_row(&p, UI_ROW_H), base + 12);

    /* Scrolling back up exposes at the top, at the new position. */
    st7735_scroll_by(&p, -UI_ROW_H);
    check("fill row after -1", st7735_scroll_fill_row(&p, -UI_ROW_H), base + 12);

    /* Across the wrap the strip may straddle the end of the region, and the
     * caller has to write it in two pieces. */
    st7735_scroll_set(&p, UI_LIST_H - 4);
    st7735_scroll_by(&p, UI_ROW_H);
    uint16_t row = st7735_scroll_fill_row(&p, UI_ROW_H);
    uint16_t y0, y1;
    uint8_t fit;
    st7735_window_raw(&p, row, UI_ROW_H, &y0, &y1, &fit);
    printf("  straddles: row %u, %u of %u rows before the wrap\n", row, fit, UI_ROW_H);
    check("split detected", fit < UI_ROW_H, 1);
    check("first piece stays in region", y1 < base + UI_LIST_H, 1);

    /* A strip well inside the region is not split. */
    st7735_window_raw(&p, base, UI_ROW_H, &y0, &y1, &fit);
    check("no split needed", fit, UI_ROW_H);
    check("y1 correct", y1, base + UI_ROW_H - 1);
}

static void test_cost(void) {
    printf("what this buys\n");

    const unsigned bpp = 2;
    unsigned full = 128u * UI_LIST_H * bpp;
    unsigned one  = 128u * UI_ROW_H * bpp;
    printf("  full list repaint %5u B   one row %4u B   ratio %.1fx\n",
           full, one, (double)full / one);
    check("a row is 8x cheaper", full / one, 8);

    st7735_panel_cfg_t p;
    st7735_panel_init(&p, ST7735_PANEL_128X128);
    unsigned spare = st7735_offpanel_rows(&p);
    printf("  %u spare rows = %.1f list rows of staging\n",
           spare, (double)spare / UI_ROW_H);
    check("nearly three rows of staging", spare / UI_ROW_H, 2);
}

int main(void) {
    test_geometry();
    test_window();
    test_scroll_setup();
    test_scroll_motion();
    test_fill_row();
    test_cost();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
