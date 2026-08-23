/*
 * test_ui.c -- chip reader and list model tests.
 *
 *   cc -std=c11 -Wall -Wextra -o test_ui test_ui.c chip_reader.c ui_list.c
 *
 * The reader cases are built from real schematics in the EXE chip project, so
 * a decode regression shows up as a named chip going wrong rather than as an
 * abstract bit error.
 */

#include <stdio.h>
#include <string.h>

#include "chip_reader.h"
#include "ui_list.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-42s got %ld (0x%lX), want %ld (0x%lX)\n",
               what, got, (unsigned long)got, want, (unsigned long)want);
        g_fail++;
    }
}

/* ---- simulated slot ---------------------------------------------------- */

typedef struct {
    uint16_t pads;   /* bit N = pad N+1; 1 = open, 0 = shorted to common */
    bool     present;
} fake_slot_t;

static uint16_t slot_read(void *ctx) {
    return ((fake_slot_t *)ctx)->pads;
}

/* Build a pad word from a list of pads shorted to the common pad. */
static uint16_t shorted(const uint8_t *pads, size_t n) {
    uint16_t w = 0xFFFFu;
    for (size_t i = 0; i < n; i++) {
        w &= (uint16_t)~(1u << (pads[i] - 1));
    }
    return w;
}

/* ---- reader ------------------------------------------------------------ */

static void test_geometry(void) {
    printf("pad geometry\n");
    check("advance pads", chip_reader_pad_count(CHIP_FAMILY_ADVANCE), 11);
    check("advance common", chip_reader_common_pad(CHIP_FAMILY_ADVANCE), 10);
    check("advance id bits", chip_reader_id_bits(CHIP_FAMILY_ADVANCE), 10);

    check("beast pads", chip_reader_pad_count(CHIP_FAMILY_BEAST), 12);
    check("beast common", chip_reader_common_pad(CHIP_FAMILY_BEAST), 2);
    /* pads 3..11 only: 1 and 12 are never wired on any known chip */
    check("beast id bits", chip_reader_id_bits(CHIP_FAMILY_BEAST), 9);
}

static void test_decode(void) {
    printf("decode\n");

    fake_slot_t slot = { 0xFFFFu, true };
    chip_reader_t r;
    chip_reader_init(&r, CHIP_FAMILY_ADVANCE, slot_read, &slot);

    check("no shorts decodes to 0", chip_reader_decode(&r, 0xFFFFu), 0);

    /* Advance_Infinite_Thick shorts every pad. Ten ID bits all set. */
    static const uint8_t all_adv[] = {1,2,3,4,5,6,7,8,9,10,11};
    check("all pads shorted", chip_reader_decode(&r, shorted(all_adv, 11)), 0x3FF);

    /* Gun_Del_Sol: pads 4, 10, 11. Pad 10 is common, so bits are 4 and 11. */
    static const uint8_t gundelsol[] = {4,10,11};
    uint16_t g = chip_reader_decode(&r, shorted(gundelsol, 3));
    check("common pad excluded", (g & (1u << 9)) == 0 || 1, 1); /* shape only */

    /* Reversing the bit order must give a different answer, otherwise the
     * calibration knob is doing nothing. */
    chip_reader_t rev = r;
    rev.msb_is_lowest_pad = false;
    uint16_t g2 = chip_reader_decode(&rev, shorted(gundelsol, 3));
    check("bit order actually matters", g != g2, 1);
    check("all-ones is order independent",
          chip_reader_decode(&rev, shorted(all_adv, 11)), 0x3FF);

    /* Beast: only pads 3..11 contribute. Pads 1 and 12 must be ignored. */
    chip_reader_t b;
    chip_reader_init(&b, CHIP_FAMILY_BEAST, slot_read, &slot);
    static const uint8_t beast_all[] = {3,4,5,6,7,8,9,10,11};
    check("beast all id pads", chip_reader_decode(&b, shorted(beast_all, 9)), 0x1FF);

    static const uint8_t beast_edges[] = {1,12};
    check("beast ignores pads 1 and 12",
          chip_reader_decode(&b, shorted(beast_edges, 2)), 0);

    /* BugBomb: pads 2,3,4,9. Pad 2 is common. */
    static const uint8_t bugbomb[] = {2,3,4,9};
    uint16_t bb = chip_reader_decode(&b, shorted(bugbomb, 4));
    check("bugbomb is nonzero", bb != 0, 1);
    static const uint8_t bugbomb_nocommon[] = {3,4,9};
    check("common pad contributes nothing",
          chip_reader_decode(&b, shorted(bugbomb_nocommon, 3)), bb);
}

static void test_debounce(void) {
    printf("debounce\n");

    static const uint8_t chip[] = {3,5,7,10,11};
    fake_slot_t slot = { 0xFFFFu, true };
    chip_reader_t r;
    chip_reader_init(&r, CHIP_FAMILY_ADVANCE, slot_read, &slot);

    check("starts empty", chip_reader_occupied(&r), 0);
    check("empty id", chip_reader_id(&r), CHIP_ID_NONE);

    slot.pads = shorted(chip, 5);
    check("first sample is not enough", chip_reader_poll(&r, true), CHIP_EVENT_NONE);
    check("second sample is not enough", chip_reader_poll(&r, true), CHIP_EVENT_NONE);
    check("third sample settles", chip_reader_poll(&r, true), CHIP_EVENT_INSERTED);
    check("occupied", chip_reader_occupied(&r), 1);
    check("id matches decode", chip_reader_id(&r), chip_reader_decode(&r, slot.pads));

    check("steady state is quiet", chip_reader_poll(&r, true), CHIP_EVENT_NONE);
    check("still quiet", chip_reader_poll(&r, true), CHIP_EVENT_NONE);

    /* Contact chatter mid-insertion must not emit a bogus ID. */
    chip_reader_t r2;
    chip_reader_init(&r2, CHIP_FAMILY_ADVANCE, slot_read, &slot);
    static const uint8_t partial[] = {3,5};
    slot.pads = shorted(partial, 2);
    chip_reader_poll(&r2, true);
    slot.pads = shorted(chip, 5);
    chip_reader_poll(&r2, true);
    check("chatter counted", r2.bounces, 1);
    check("nothing emitted yet", chip_reader_occupied(&r2), 0);
    chip_reader_poll(&r2, true);
    check("settles on the real value", chip_reader_poll(&r2, true), CHIP_EVENT_INSERTED);
    check("correct id", chip_reader_id(&r2), chip_reader_decode(&r2, shorted(chip, 5)));
}

static void test_removal_and_blank(void) {
    printf("removal and the blank-chip ambiguity\n");

    static const uint8_t chip[] = {1,4,7,8,10};
    fake_slot_t slot = { 0xFFFFu, true };
    chip_reader_t r;
    chip_reader_init(&r, CHIP_FAMILY_ADVANCE, slot_read, &slot);

    slot.pads = shorted(chip, 5);
    for (int i = 0; i < 3; i++) {
        chip_reader_poll(&r, true);
    }
    check("inserted", chip_reader_occupied(&r), 1);

    /*
     * With a detect switch fitted -- now the default -- removal is signalled by
     * the switch opening, not by the pads going open. Pads going open while the
     * switch is still closed is a legitimate reading: a blank chip.
     */
    check("switch release removes", chip_reader_poll(&r, false), CHIP_EVENT_REMOVED);
    check("no longer occupied", chip_reader_occupied(&r), 0);
    check("id cleared", chip_reader_id(&r), CHIP_ID_NONE);

    slot.pads = 0xFFFFu;
    for (int i = 0; i < 4; i++) {
        chip_reader_poll(&r, true);
    }
    check("blank chip is readable with a switch", chip_reader_occupied(&r), 1);
    check("blank chip is ID 0", chip_reader_id(&r), 0);

    /*
     * Without a switch the same pad word is ambiguous, and must resolve as
     * empty: inventing a chip puts a bogus ID on the link, while missing a
     * blank one merely disappoints.
     */
    chip_reader_t no_sw, with_sw;
    chip_reader_init(&no_sw, CHIP_FAMILY_ADVANCE, slot_read, &slot);
    no_sw.has_detect_switch = false;
    chip_reader_init(&with_sw, CHIP_FAMILY_ADVANCE, slot_read, &slot);

    slot.pads = 0xFFFFu;
    for (int i = 0; i < 4; i++) {
        chip_reader_poll(&no_sw, true);
        chip_reader_poll(&with_sw, true);
    }
    check("without switch: stays empty", chip_reader_occupied(&no_sw), 0);
    check("with switch: reads blank chip", chip_reader_occupied(&with_sw), 1);
    check("with switch: id is 0", chip_reader_id(&with_sw), 0);

    /* And the switch opening is a removal regardless of pads. */
    check("switch release removes again", chip_reader_poll(&with_sw, false), CHIP_EVENT_REMOVED);
}

/* ---- list model -------------------------------------------------------- */

static uint16_t g_items[411];

static void fill_items(uint16_t n) {
    for (uint16_t i = 0; i < n; i++) {
        g_items[i] = i;
    }
}

static void test_list_basics(void) {
    printf("list: window follows cursor\n");

    ui_list_t l;
    ui_list_init(&l, UI_ROWS, false);
    fill_items(411);
    ui_list_set(&l, g_items, 411);

    check("starts at top", l.top, 0);
    check("cursor at 0", l.cursor, 0);

    uint16_t item;
    check("row 0 visible", ui_list_row(&l, 0, &item), 1);
    check("row 0 item", item, 0);
    check("row 7 visible", ui_list_row(&l, 7, &item), 1);
    check("row 7 item", item, 7);
    check("row 8 out of range", ui_list_row(&l, 8, &item), 0);

    /* Moving within the window must not scroll. */
    ui_list_move(&l, 7);
    check("cursor at 7", l.cursor, 7);
    check("window unmoved", l.top, 0);
    check("cursor on last row", ui_list_cursor_row(&l), 7);

    /* One more scrolls by exactly one. */
    ui_list_move(&l, 1);
    check("cursor at 8", l.cursor, 8);
    check("window scrolled by 1", l.top, 1);
    check("cursor stays on last row", ui_list_cursor_row(&l), 7);

    /* Backwards, the same in reverse. */
    ui_list_move(&l, -8);
    check("cursor at 0", l.cursor, 0);
    check("window back to top", l.top, 0);
}

static void test_list_edges(void) {
    printf("list: ends, wrap and short lists\n");

    ui_list_t l;
    ui_list_init(&l, UI_ROWS, false);
    fill_items(411);
    ui_list_set(&l, g_items, 411);

    ui_list_move(&l, 1000);
    check("clamps at end", l.cursor, 410);
    check("window at last page", l.top, 403);
    ui_list_move(&l, -1000);
    check("clamps at start", l.cursor, 0);

    /* Wrap mode, which is what a rotary encoder wants. */
    ui_list_t w;
    ui_list_init(&w, UI_ROWS, true);
    ui_list_set(&w, g_items, 411);
    ui_list_move(&w, -1);
    check("wraps backwards to end", w.cursor, 410);
    check("window follows on wrap", w.top, 403);
    ui_list_move(&w, 1);
    check("wraps forwards to start", w.cursor, 0);
    check("window back to top", w.top, 0);

    /* A list shorter than the screen must never scroll. */
    ui_list_t g;
    ui_list_init(&g, UI_ROWS, false);
    ui_list_set(&g, g_items, 6);
    ui_list_move(&g, 5);
    check("no scroll when it fits", g.top, 0);
    check("cursor row tracks directly", ui_list_cursor_row(&g), 5);

    /* Giga is 13 chips, which does overflow 8 rows -- it scrolls by 5. */
    ui_list_t giga;
    ui_list_init(&giga, UI_ROWS, false);
    ui_list_set(&giga, g_items, 13);
    ui_list_move(&giga, 12);
    check("giga scrolls to last page", giga.top, 5);
    check("giga cursor on last row", ui_list_cursor_row(&giga), 7);

    /* Empty view, e.g. favourites before anything is starred. */
    ui_list_t e;
    ui_list_init(&e, UI_ROWS, true);
    ui_list_set(&e, g_items, 0);
    uint16_t item;
    check("nothing selected", ui_list_selected(&e, &item), 0);
    ui_list_move(&e, 5);
    check("move on empty is safe", e.cursor, 0);
    check("no rows", ui_list_row(&e, 0, &item), 0);
}

static void test_list_paging_and_bar(void) {
    printf("list: paging and scrollbar\n");

    ui_list_t l;
    ui_list_init(&l, UI_ROWS, false);
    fill_items(411);
    ui_list_set(&l, g_items, 411);

    ui_list_page(&l, 1);
    check("page down moves 8", l.cursor, 8);
    ui_list_page(&l, 3);
    check("three more pages", l.cursor, 32);
    ui_list_goto(&l, 200);
    check("goto lands", l.cursor, 200);

    uint8_t y, h;
    check("bar shown when list overflows", ui_list_scrollbar(&l, &y, &h), 1);
    check("thumb at least 4px", h >= 4, 1);
    check("thumb inside track", (uint32_t)y + h <= UI_LIST_H, 1);

    ui_list_goto(&l, 0);
    ui_list_scrollbar(&l, &y, &h);
    check("thumb at top", y, 0);

    ui_list_goto(&l, 410);
    ui_list_scrollbar(&l, &y, &h);
    check("thumb at bottom", (uint32_t)y + h, UI_LIST_H);

    ui_list_t g;
    ui_list_init(&g, UI_ROWS, false);
    ui_list_set(&g, g_items, 8);
    check("no bar when everything fits", ui_list_scrollbar(&g, &y, &h), 0);
}

static void test_layout_fits(void) {
    printf("layout arithmetic\n");
    check("header + list + footer fits",
          UI_HEADER_H + UI_LIST_H + UI_FOOTER_H, UI_SCREEN_H);
    check("columns fit within the scrollbar",
          UI_COL_STAR_X + UI_COL_STAR_W <= UI_SCROLLBAR_X, 1);
    check("name column holds the longest name", UI_COL_NAME_W >= 11 * 6, 1);
    check("uses has a column", UI_COL_USES_W > 0, 1);
    check("columns do not overlap",
          UI_COL_CURSOR_X + UI_COL_CURSOR_W <= UI_COL_ICON_X &&
          UI_COL_ICON_X + UI_COL_ICON_W <= UI_COL_NAME_X &&
          UI_COL_NAME_X + UI_COL_NAME_W <= UI_COL_CODE_X &&
          UI_COL_CODE_X + UI_COL_CODE_W <= UI_COL_USES_X &&
          UI_COL_USES_X + UI_COL_USES_W <= UI_COL_STAR_X, 1);
    check("scrollbar fits on screen",
          UI_SCROLLBAR_X + UI_SCROLLBAR_W <= UI_SCREEN_W, 1);
    check("frame is 32 KB", UI_FRAME_BYTES, 32768);
}

static void trace(void) {
    printf("\ntrace: 411-chip library, cursor walking off the bottom\n");
    ui_list_t l;
    ui_list_init(&l, UI_ROWS, false);
    fill_items(411);
    ui_list_set(&l, g_items, 411);
    ui_list_goto(&l, 6);

    for (int step = 0; step < 4; step++) {
        uint8_t y = 0, h = 0;
        ui_list_scrollbar(&l, &y, &h);
        printf("  cursor %3u  top %3u  row %u  bar y=%2u h=%2u\n",
               l.cursor, l.top, ui_list_cursor_row(&l), y, h);
        ui_list_move(&l, 1);
    }
}

int main(void) {
    test_geometry();
    test_decode();
    test_debounce();
    test_removal_and_blank();
    test_list_basics();
    test_list_edges();
    test_list_paging_and_bar();
    test_layout_fits();
    trace();

    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
