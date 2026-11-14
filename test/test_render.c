/*
 * test_render.c -- render the chip screen into a framebuffer and check it.
 *
 *   cc -std=c11 -Wall -Wextra -o test_render test_render.c ui_render.c \
 *      ui_font.c ui_chipview.c ui_list.c chipdb.c
 *   ./test_render chipdb/bn6g.chdb
 *
 * Writes screen*.ppm so the layout can be looked at, and asserts on the pixels
 * so a regression fails the build rather than waiting to be noticed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_font.h"
#include "ui_render.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-42s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static chipdb_t g_db;
static uint16_t g_view[512];
static uint16_t g_fb[UI_SCREEN_W * UI_SCREEN_H];
static ui_canvas_t g_canvas = { g_fb, UI_SCREEN_W, UI_SCREEN_H };
static void *g_blob;

static uint16_t px(int x, int y) { return g_fb[(size_t)y * UI_SCREEN_W + x]; }

/*
 * A pixel guaranteed to be row background: column 5 sits between the cursor
 * glyph (0..4) and the element block (7..12), and y+10 is below the 7px font
 * baseline. Sampling anywhere else risks landing on ink and testing the wrong
 * thing.
 */
static uint16_t row_bg(int row) {
    return px(5, UI_LIST_Y + row * UI_ROW_H + 10);
}

static void save_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", UI_SCREEN_W, UI_SCREEN_H);
    for (int i = 0; i < UI_SCREEN_W * UI_SCREEN_H; i++) {
        uint16_t c = g_fb[i];
        uint8_t rgb[3] = {
            (uint8_t)(((c >> 11) & 0x1F) * 255 / 31),
            (uint8_t)(((c >> 5) & 0x3F) * 255 / 63),
            (uint8_t)((c & 0x1F) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static bool load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_blob = malloc((size_t)n);
    size_t rd = fread(g_blob, 1, (size_t)n, f);
    fclose(f);
    return rd == (size_t)n && chipdb_open(&g_db, g_blob, (size_t)n);
}

/* ---- font -------------------------------------------------------------- */

static void test_font(void) {
    printf("font\n");
    check("advance fits 21 columns", UI_SCREEN_W / UI_FONT_ADVANCE, 21);
    check("11-char name fits the column",
          ui_font_width("ProtoMn[EX]") <= UI_COL_NAME_W, 1);
    check("width of one glyph", ui_font_width("A"), UI_FONT_W);
    check("width of two", ui_font_width("AB"), UI_FONT_W + UI_FONT_ADVANCE);
    check("empty is zero", ui_font_width(""), 0);

    /* 1 glyph is 5px, each further one adds 6: 5, 11, 17, 23. */
    check("fit clips by glyph", ui_font_fit("ABCDEFGHIJ", 20), 3);
    check("exact fit", ui_font_fit("ABCDEFGHIJ", 23), 4);
    check("fit of nothing", ui_font_fit("ABC", 2), 0);
    check("fit of everything", ui_font_fit("ABC", 200), 3);

    /* Every glyph a chip name can contain must have ink, or names render with
     * silent holes. Chip names use letters, digits, '-', '[' and ']'. */
    const char *needed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-[]+*.";
    for (const char *p = needed; *p; p++) {
        const uint8_t *g = ui_font_glyph(*p);
        int ink = 0;
        for (int i = 0; i < UI_FONT_W; i++) ink |= g[i];
        if (!ink) {
            printf("  FAIL glyph '%c' is blank\n", *p);
            g_fail++;
        }
    }
    /*
     * Descenders below the baseline, and nothing else there. g, p, q and y
     * used to fit their tails inside the seven rows, which lifted their
     * bodies a row above every other letter's: "game" read as "9ame".
     */
    for (int c = UI_FONT_FIRST; c <= UI_FONT_LAST; c++) {
        const uint8_t *gl = ui_font_glyph((char)c);
        int below = 0, x_top = 0;
        for (int i = 0; i < UI_FONT_W; i++) {
            below |= gl[i] & 0x80;
            x_top |= gl[i] & 0x02;    /* row 1: above the x-height */
        }
        bool desc = c == 'g' || c == 'j' || c == 'p' || c == 'q' || c == 'y';
        if (!!below != desc) {
            printf("  FAIL glyph '%c' %s the descender row\n", c,
                   desc ? "does not reach" : "spills into");
            g_fail++;
        }
        if (desc && c != 'j' && x_top) {
            printf("  FAIL glyph '%c' starts above the x-height\n", c);
            g_fail++;
        }
    }
    /* Every band a line of text sits in leaves room for the descender. */
    check("row: descender inside", (UI_ROW_H - UI_FONT_H) / 2 + UI_FONT_ROWS <= UI_ROW_H, 1);
    check("header: descender inside",
          (UI_HEADER_H - UI_FONT_H) / 2 + UI_FONT_ROWS <= UI_HEADER_H, 1);
    check("footer: descender inside",
          (UI_FOOTER_H - UI_FONT_H) / 2 + UI_FONT_ROWS <= UI_FOOTER_H, 1);

    /* Space must be the exception. */
    const uint8_t *sp = ui_font_glyph(' ');
    int ink = 0;
    for (int i = 0; i < UI_FONT_W; i++) ink |= sp[i];
    check("space is blank", ink, 0);
    check("unprintable falls back to space", ui_font_glyph('\n')[0], 0);
}

/* ---- primitives -------------------------------------------------------- */

static void test_clipping(void) {
    printf("clipping\n");
    memset(g_fb, 0, sizeof g_fb);

    /* Drawing off every edge must not corrupt memory or wrap to the far side. */
    ui_fill(&g_canvas, -10, -10, 5, 5, 0xFFFF);
    ui_fill(&g_canvas, UI_SCREEN_W + 5, 0, 10, 10, 0xFFFF);
    ui_text(&g_canvas, -200, 60, "OFFSCREEN", 0xFFFF);
    ui_text(&g_canvas, 60, -200, "ABOVE", 0xFFFF);

    long lit = 0;
    for (int i = 0; i < UI_SCREEN_W * UI_SCREEN_H; i++) {
        if (g_fb[i]) lit++;
    }
    check("nothing drawn off-canvas", lit, 0);

    /* Straddling an edge draws the visible part only, rather than wrapping. */
    ui_fill(&g_canvas, -2, 10, 5, 2, 0xFFFF);
    check("left straddle clipped", px(0, 10) != 0 && px(3, 10) == 0, 1);
    check("did not wrap to the right edge", px(UI_SCREEN_W - 1, 10), 0);

    /* Text starting off the left edge still shows its tail. */
    memset(g_fb, 0, sizeof g_fb);
    ui_text(&g_canvas, -12, 60, "AB CD", 0xFFFF);
    long tail = 0;
    for (int x = 0; x < 20; x++) {
        for (int y = 60; y < 60 + UI_FONT_H; y++) if (px(x, y)) tail++;
    }
    check("partially visible text still draws", tail > 0, 1);
}

/* ---- the screen -------------------------------------------------------- */

static void render(chipview_t *v, ui_list_t *l, const char *status) {
    ui_list_set(l, v->view, v->view_count);
    memset(g_fb, 0, sizeof g_fb);
    ui_render_chip_screen(&g_canvas, v, &g_db, l, status);
}

static void test_screen(void) {
    printf("chip screen\n");

    chipview_t v;
    chipview_init(&v, &g_db, g_view, 512);
    chipview_set_tab(&v, CHIP_TAB_STANDARD);

    ui_list_t l;
    ui_list_init(&l, UI_ROWS, true);
    render(&v, &l, "READY");

    /* Every region must be painted: an unpainted band shows as a black stripe
     * because the framebuffer is not cleared between frames on hardware. */
    check("header painted", px(64, 4), COL_HEADER);
    check("footer painted", px(64, UI_SCREEN_H - 4) != 0, 1);
    check("scrollbar track painted", px(UI_SCROLLBAR_X + 1, UI_LIST_Y + 90) != 0, 1);
    check("thumb at top", px(UI_SCROLLBAR_X + 1, UI_LIST_Y + 1), COL_BAR_THUMB);

    /* Selected row is highlighted; the one below it is not. */
    check("row 0 selected", row_bg(0), COL_HEADER);
    check("row 1 not selected", row_bg(1) != COL_HEADER, 1);
    check("rows alternate", row_bg(1) != row_bg(2), 1);

    ui_list_move(&l, 3);
    memset(g_fb, 0, sizeof g_fb);
    ui_render_chip_screen(&g_canvas, &v, &g_db, &l, "READY");
    check("selection moved", row_bg(3), COL_HEADER);
    check("row 0 released", row_bg(0) != COL_HEADER, 1);

    save_ppm("screen_standard.ppm");
}

static void test_element_tint(void) {
    printf("element tint\n");

    chipview_t v;
    chipview_init(&v, &g_db, g_view, 512);
    chipview_set_tab(&v, CHIP_TAB_STANDARD);
    chipview_set_sort(&v, CHIP_SORT_ELEMENT);

    ui_list_t l;
    ui_list_init(&l, UI_ROWS, true);
    render(&v, &l, NULL);

    /*
     * Assert the icon actually matches each row's element rather than merely
     * counting distinct colours: with an element sort the first eight rows are
     * all one element, so a count would pass on an all-grey render.
     */
    int checked = 0;
    for (int r = 0; r < UI_ROWS; r++) {
        uint16_t item;
        if (!ui_list_row(&l, (uint8_t)r, &item)) break;
        chip_record_t rec;
        if (!chipdb_get(&g_db, chipview_slot_at(&v, (uint16_t)(l.top + r)), &rec)) break;
        uint16_t want = ui_element_colour(rec.element);
        uint16_t got = px(UI_COL_ICON_X + 3, UI_LIST_Y + r * UI_ROW_H + 5);
        if (got != want) {
            printf("  FAIL row %d icon: got %04X want %04X\n", r, got, want);
            g_fail++;
            break;
        }
        checked++;
    }
    check("all visible icons verified", checked, UI_ROWS);

    check("colour maps", ui_element_colour(CHIP_ELEM_FIRE), COL_ELEM_FIRE);
    check("unknown falls back", ui_element_colour(99), COL_ELEM_NONE);
}

static void test_favorites_and_move(void) {
    printf("favourites and move mode\n");

    chipview_t v;
    chipview_init(&v, &g_db, g_view, 512);
    chipview_set_tab(&v, CHIP_TAB_STANDARD);

    ui_list_t l;
    ui_list_init(&l, UI_ROWS, true);
    render(&v, &l, NULL);

    int star_y = UI_LIST_Y + UI_ROW_H + 3;
    check("row 1 star is dim", px(UI_COL_STAR_X + 2, star_y) != COL_STAR_ON, 1);

    chipview_toggle_favorite(&v, chipview_slot_at(&v, 1));
    render(&v, &l, NULL);
    bool gold = false;
    for (int y = UI_LIST_Y + UI_ROW_H; y < UI_LIST_Y + 2 * UI_ROW_H; y++) {
        for (int x = UI_COL_STAR_X; x < UI_COL_STAR_X + UI_COL_STAR_W; x++) {
            if (px(x, y) == COL_STAR_ON) gold = true;
        }
    }
    check("starred row shows gold", gold, 1);
    save_ppm("screen_favorite.ppm");

    /* A chip picked up in Move mode must be visibly distinct. */
    chipview_set_mode(&v, CHIP_MODE_MOVE);
    chipview_pick(&v, 2);
    render(&v, &l, "MOVE");
    check("held row tinted", row_bg(2), COL_HELD);
    save_ppm("screen_move.ppm");
}

static void test_header_and_footer(void) {
    printf("header and footer\n");

    chipview_t v;
    chipview_init(&v, &g_db, g_view, 512);
    ui_list_t l;
    ui_list_init(&l, UI_ROWS, true);

    /* Dark is greyed on BN6 Gregar, so its pip must differ from an available
     * one -- that is the whole point of drawing pips instead of omitting. */
    render(&v, &l, NULL);
    uint16_t dark_pip = px(70 + CHIP_TAB_DARK * 9 + 2, 4);
    uint16_t mega_pip = px(70 + CHIP_TAB_MEGA * 9 + 2, 4);
    check("greyed pip is dim", dark_pip, COL_TEXT_DIM);
    check("available pip is not", mega_pip != COL_TEXT_DIM, 1);

    /* A long status must not overwrite the mode label. */
    render(&v, &l, "SENT HiCannon x3 OK LONGER");
    int ty = UI_SCREEN_H - UI_FOOTER_H + (UI_FOOTER_H - UI_FONT_H) / 2;
    bool mode_ink = false;
    for (int x = 2; x < 2 + (int)ui_font_width("SLOT-IN"); x++) {
        for (int y = ty; y < ty + UI_FONT_H; y++) {
            if (px(x, y) == COL_CURSOR) mode_ink = true;
        }
    }
    check("mode label survives a long status", mode_ink, 1);
    save_ppm("screen_footer.ppm");
}

/*
 * The renderer maps rows to chipview positions through the list's scroll
 * offset. If the list is backed by some other array, every row would show a
 * real chip in the wrong place -- wrong data that looks entirely plausible.
 */
static void test_list_mismatch(void) {
    printf("mismatched list and view\n");

    chipview_t v;
    chipview_init(&v, &g_db, g_view, 512);
    chipview_set_tab(&v, CHIP_TAB_STANDARD);

    ui_list_t l;
    ui_list_init(&l, UI_ROWS, true);
    ui_list_set(&l, v.view, v.view_count);
    check("matched pair accepted", ui_render_list_matches(&v, &l), 1);

    /* Same contents, different array: still a mismatch, because the renderer
     * cannot know the orders agree. */
    static uint16_t other[512];
    memcpy(other, v.view, sizeof(uint16_t) * v.view_count);
    ui_list_set(&l, other, v.view_count);
    check("foreign array rejected", ui_render_list_matches(&v, &l), 0);

    memset(g_fb, 0, sizeof g_fb);
    ui_render_chip_screen(&g_canvas, &v, &g_db, &l, "READY");
    check("list painted blank", row_bg(0), COL_BG);
    check("row 3 blank too", row_bg(3), COL_BG);
    check("header still drawn", px(64, 4), COL_HEADER);

    /* A stale count is a mismatch too: the view shrank when a tab changed. */
    ui_list_set(&l, v.view, v.view_count);
    check("matched again", ui_render_list_matches(&v, &l), 1);
    chipview_set_tab(&v, CHIP_TAB_GIGA);
    check("stale count rejected", ui_render_list_matches(&v, &l), 0);
}

static void test_empty(void) {
    printf("empty list\n");

    chipview_t v;
    chipview_init(&v, &g_db, g_view, 512);
    ui_list_t l;
    ui_list_init(&l, UI_ROWS, true);
    ui_list_set(&l, v.view, 0);

    memset(g_fb, 0, sizeof g_fb);
    ui_render_chip_screen(&g_canvas, &v, &g_db, &l, "NO CHIPS");

    /* Blank rows must still be painted, and no scrollbar thumb should appear. */
    check("blank rows painted", px(40, UI_LIST_Y + 40), COL_BG);
    check("blank row background", row_bg(4), COL_BG);
    bool thumb = false;
    for (int y = UI_LIST_Y; y < UI_LIST_Y + UI_LIST_H; y++) {
        if (px(UI_SCROLLBAR_X + 1, y) == COL_BAR_THUMB) thumb = true;
    }
    check("no thumb when empty", thumb, 0);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "chipdb/bn6g.chdb";
    if (!load(path)) {
        printf("cannot open %s\n", path);
        return 2;
    }
    printf("rendering from %s\n\n", path);

    test_font();
    test_clipping();
    test_screen();
    test_element_tint();
    test_favorites_and_move();
    test_header_and_footer();
    test_list_mismatch();
    test_empty();

    free(g_blob);
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
