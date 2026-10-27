/*
 * ui_render.c -- see ui_render.h
 */

#include <string.h>

#include "ui_font.h"
#include "ui_render.h"

/* ---- primitives ------------------------------------------------------- */

static inline bool in_bounds(const ui_canvas_t *c, int x, int y) {
    return x >= 0 && y >= 0 && x < (int)c->width && y < (int)c->height;
}

void ui_fill(const ui_canvas_t *c, int x, int y, int w, int h, uint16_t colour) {
    if (!c || !c->fb) {
        return;
    }
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= (int)c->height) {
            continue;
        }
        for (int col = x; col < x + w; col++) {
            if (col < 0 || col >= (int)c->width) {
                continue;
            }
            c->fb[(size_t)row * c->width + col] = colour;
        }
    }
}

static void draw_glyph(const ui_canvas_t *c, int x, int y, char ch, uint16_t colour) {
    const uint8_t *cols = ui_font_glyph(ch);
    for (int gx = 0; gx < UI_FONT_W; gx++) {
        uint8_t bits = cols[gx];
        for (int gy = 0; gy < UI_FONT_ROWS; gy++) {
            if (!(bits & (1u << gy))) {
                continue;
            }
            int px = x + gx, py = y + gy;
            if (in_bounds(c, px, py)) {
                c->fb[(size_t)py * c->width + px] = colour;
            }
        }
    }
}

void ui_text(const ui_canvas_t *c, int x, int y, const char *s, uint16_t colour) {
    if (!s) {
        return;
    }
    for (int i = 0; s[i]; i++) {
        draw_glyph(c, x + i * UI_FONT_ADVANCE, y, s[i], colour);
    }
}

/*
 * Clip by whole glyphs rather than mid-glyph. A half-drawn letter at a column
 * boundary reads as a rendering bug; a truncated word reads as truncation.
 */
void ui_text_clipped(const ui_canvas_t *c, int x, int y, int max_w,
                     const char *s, uint16_t colour) {
    if (!s || max_w <= 0) {
        return;
    }
    uint16_t n = ui_font_fit(s, (uint16_t)max_w);
    for (uint16_t i = 0; i < n; i++) {
        draw_glyph(c, x + (int)i * UI_FONT_ADVANCE, y, s[i], colour);
    }
}

uint16_t ui_element_colour(uint8_t element) {
    switch (element) {
    case CHIP_ELEM_FIRE: return COL_ELEM_FIRE;
    case CHIP_ELEM_AQUA: return COL_ELEM_AQUA;
    case CHIP_ELEM_ELEC: return COL_ELEM_ELEC;
    case CHIP_ELEM_WOOD: return COL_ELEM_WOOD;
    default:             return COL_ELEM_NONE;
    }
}

/* ---- one list row ----------------------------------------------------- */

bool ui_render_row(const ui_canvas_t *c, const chipview_t *v, const chipdb_t *db,
                   uint8_t visible_row, uint16_t item_index, bool selected) {
    int y = UI_LIST_Y + visible_row * UI_ROW_H;

    uint16_t slot = chipview_slot_at(v, item_index);
    chip_record_t rec;
    if (slot == CHIPVIEW_NONE || !chipdb_get(db, slot, &rec)) {
        ui_fill(c, 0, y, UI_SCREEN_W, UI_ROW_H, COL_BG);
        return false;
    }

    /* Alternating bands make an eight-row list scannable without gridlines. */
    uint16_t bg = (visible_row & 1) ? COL_ROW_ALT : COL_BG;
    if (selected) {
        bg = COL_HEADER;
    }
    if (chipview_held(v) == item_index) {
        bg = COL_HELD; /* picked up in Move mode */
    }
    ui_fill(c, 0, y, UI_SCROLLBAR_X, UI_ROW_H, bg);

    int ty = y + (UI_ROW_H - UI_FONT_H) / 2; /* centre the 7px font in 12px */
    /* Text on the header colour or the held colour uses on_header: those
     * are the two bright bands, and body text on them is unreadable in most
     * themes. */
    bool bright = selected || chipview_held(v) == item_index;
    uint16_t fg = bright ? COL_ON_HEADER : COL_TEXT;

    if (selected) {
        draw_glyph(c, UI_COL_CURSOR_X, ty, '>', COL_ON_HEADER);
    }

    /* Element as a small block, which is all there is room for at 10px. */
    ui_fill(c, UI_COL_ICON_X + 1, y + 3, 6, 6, ui_element_colour(rec.element));

    ui_text_clipped(c, UI_COL_NAME_X, ty, UI_COL_NAME_W,
                    chipdb_name(db, slot), fg);

    /* Code box, tinted by element the way the homebrew does it. */
    char code = chipdb_code_char(rec.codes[0]);
    if (code) {
        ui_fill(c, UI_COL_CODE_X, y + 1, 9, UI_ROW_H - 2,
                ui_element_colour(rec.element));
        draw_glyph(c, UI_COL_CODE_X + 2, ty, code, COL_ON_ELEMENT);
    }

    /* Uses: the Chip Gate Use Limit, 1 or 3. */
    if (rec.gate_uses) {
        char u[2] = { (char)('0' + (rec.gate_uses % 10)), '\0' };
        ui_text(c, UI_COL_USES_X + 1, ty, u, fg);
    }

    bool fav = chipview_is_favorite(v, slot);
    draw_glyph(c, UI_COL_STAR_X, ty, '*', fav ? COL_STAR_ON : COL_STAR_OFF);
    return true;
}

/* ---- header ----------------------------------------------------------- */

/*
 * Six tab names side by side need 132px and we have 128, so the homebrew's
 * full strip does not fit. Show the active tab by name with arrows, plus a row
 * of pips for position -- same information, a quarter of the width. Pips for
 * unavailable libraries are drawn dim rather than omitted, so the strip does
 * not change length as favourites come and go.
 */
void ui_render_header(const ui_canvas_t *c, const chipview_t *v) {
    ui_fill(c, 0, 0, UI_SCREEN_W, UI_HEADER_H, COL_HEADER);
    int ty = (UI_HEADER_H - UI_FONT_H) / 2;

    draw_glyph(c, 1, ty, '<', COL_ON_HEADER);
    /* 47px: "Standard", eight glyphs, with the arrows either side. */
    ui_text_clipped(c, 9, ty, 47, chipview_tab_name(v->tab), COL_ON_HEADER);
    draw_glyph(c, 58, ty, '>', COL_ON_HEADER);

    for (int t = 0; t < CHIP_TAB_COUNT; t++) {
        int px = 70 + t * 9;
        bool avail = chipview_tab_available(v, (chip_tab_t)t);
        uint16_t colour = (t == (int)v->tab) ? COL_ON_HEADER
                        : avail               ? COL_TEXT
                                              : COL_TEXT_DIM;
        ui_fill(c, px, ty + 1, 5, 5, colour);
    }
}

/* ---- footer ----------------------------------------------------------- */

uint16_t ui_status_colour(const char *status) {
    /* The gate's own words (gate_app.c): a chip on its way is good news, a
     * wait or a refusal is worth a glance, a dead link is the one to act on. */
    static const struct { const char *prefix; int role; } k[] = {
        { "SENT", 1 }, { "LINKED", 1 }, { "TEST CHIP", 1 },
        { "BUSY", 2 }, { "WAITING", 2 }, { "CANCELLED", 2 },
        { "LINK STALLED", 3 },
    };
    for (unsigned i = 0; status && i < sizeof k / sizeof k[0]; i++) {
        if (strncmp(status, k[i].prefix, strlen(k[i].prefix)) == 0) {
            return k[i].role == 1 ? COL_GOOD : k[i].role == 2 ? COL_WARN : COL_BAD;
        }
    }
    return COL_TEXT;
}

void ui_render_footer(const ui_canvas_t *c, const chipview_t *v,
                      const char *status) {
    int y = UI_SCREEN_H - UI_FOOTER_H;
    ui_fill(c, 0, y, UI_SCREEN_W, UI_FOOTER_H, COL_ROW_ALT);
    ui_fill(c, 0, y, UI_SCREEN_W, 1, COL_HEADER);

    int ty = y + (UI_FOOTER_H - UI_FONT_H) / 2;
    const char *mode = chipview_mode_name(v->mode);
    ui_text(c, 2, ty, mode, COL_CURSOR);

    if (status) {
        /* Right-align, and clip from the left edge of the free space so the
         * mode label is never overwritten. */
        int left = 2 + (int)ui_font_width(mode) + 4;
        int avail = UI_SCREEN_W - 2 - left;
        if (avail > 0) {
            int w = (int)ui_font_width(status);
            int x = (w < avail) ? UI_SCREEN_W - 2 - w : left;
            ui_text_clipped(c, x, ty, avail, status, ui_status_colour(status));
        }
    }
}

/* ---- scrollbar -------------------------------------------------------- */

void ui_render_scrollbar(const ui_canvas_t *c, const ui_list_t *l) {
    ui_fill(c, UI_SCROLLBAR_X, UI_LIST_Y, UI_SCROLLBAR_W, UI_LIST_H,
            COL_BAR_TRACK);

    uint8_t y = 0, h = 0;
    if (ui_list_scrollbar(l, &y, &h)) {
        ui_fill(c, UI_SCROLLBAR_X, UI_LIST_Y + y, UI_SCROLLBAR_W, h,
                COL_BAR_THUMB);
    }
}

/* ---- menus ------------------------------------------------------------- */

void ui_render_menu(const ui_canvas_t *c, const char *title,
                    ui_label_fn label_at, void *ctx,
                    uint8_t count, uint8_t cursor, const char *footer) {
    ui_fill(c, 0, 0, UI_SCREEN_W, UI_SCREEN_H, COL_BG);

    ui_fill(c, 0, 0, UI_SCREEN_W, UI_HEADER_H, COL_HEADER);
    ui_text_clipped(c, 3, (UI_HEADER_H - UI_FONT_H) / 2, UI_SCREEN_W - 6,
                    title, COL_ON_HEADER);

    /*
     * Scroll the window with the cursor rather than clipping the list, so a
     * menu longer than the screen is still reachable. Modes fit today; the
     * game list plus a future entry would not.
     */
    uint8_t rows = UI_ROWS;
    uint8_t top = 0;
    if (count > rows && cursor >= rows) {
        top = (uint8_t)(cursor - rows + 1);
    }

    for (uint8_t row = 0; row < rows; row++) {
        uint8_t index = (uint8_t)(top + row);
        int y = UI_LIST_Y + row * UI_ROW_H;
        const char *label = (index < count) ? label_at(ctx, index) : NULL;
        if (!label) {
            ui_fill(c, 0, y, UI_SCREEN_W, UI_ROW_H, COL_BG);
            continue;
        }

        bool selected = (index == cursor);
        ui_fill(c, 0, y, UI_SCREEN_W, UI_ROW_H,
                selected ? COL_HEADER : ((row & 1) ? COL_ROW_ALT : COL_BG));

        int ty = y + (UI_ROW_H - UI_FONT_H) / 2;
        if (selected) {
            draw_glyph(c, 2, ty, '>', COL_ON_HEADER);
        }
        ui_text_clipped(c, 12, ty, UI_SCREEN_W - 16, label,
                        selected ? COL_ON_HEADER : COL_TEXT);
    }

    int fy = UI_SCREEN_H - UI_FOOTER_H;
    ui_fill(c, 0, fy, UI_SCREEN_W, UI_FOOTER_H, COL_ROW_ALT);
    ui_fill(c, 0, fy, UI_SCREEN_W, 1, COL_HEADER);
    if (footer) {
        /* "left\tright": what confirm does, then Back right-aligned and dim
         * (ui_text.h). A footer without a tab is all left. */
        int ty = fy + (UI_FOOTER_H - UI_FONT_H) / 2;
        const char *tab = strchr(footer, '\t');
        int rw = 0;
        if (tab && tab[1]) {
            rw = (int)ui_font_width(tab + 1);
            ui_text(c, UI_SCREEN_W - 2 - rw, ty, tab + 1, COL_TEXT_DIM);
        }
        char left[48];
        size_t n = tab ? (size_t)(tab - footer) : strlen(footer);
        if (n > sizeof left - 1) n = sizeof left - 1;
        memcpy(left, footer, n);
        left[n] = '\0';
        ui_text_clipped(c, 2, ty, UI_SCREEN_W - 4 - (rw ? rw + 6 : 0), left,
                        tab ? COL_TEXT : COL_TEXT_DIM);
    }
}

/* ---- whole screen ----------------------------------------------------- */

bool ui_render_list_matches(const chipview_t *v, const ui_list_t *l) {
    return v && l && l->items == v->view && l->count == v->view_count;
}

void ui_render_chip_screen(const ui_canvas_t *c, const chipview_t *v,
                           const chipdb_t *db, const ui_list_t *l,
                           const char *status) {
    ui_render_header(c, v);

    /*
     * Rows are addressed by chipview position derived from the list's scroll
     * offset, which only means anything if the list is backed by this view.
     * Drawing from a mismatched pair would show real chip names in the wrong
     * order -- wrong data that looks right. Blank is the honest failure.
     */
    if (!ui_render_list_matches(v, l)) {
        ui_fill(c, 0, UI_LIST_Y, UI_SCROLLBAR_X, UI_LIST_H, COL_BG);
        ui_render_scrollbar(c, l);
        ui_render_footer(c, v, "LIST MISMATCH");
        return;
    }

    for (uint8_t row = 0; row < UI_ROWS; row++) {
        uint16_t item;
        if (!ui_list_row(l, row, &item)) {
            ui_fill(c, 0, UI_LIST_Y + row * UI_ROW_H, UI_SCROLLBAR_X,
                    UI_ROW_H, COL_BG);
            continue;
        }
        /* ui_list stores view positions, which is what chipview indexes by. */
        ui_render_row(c, v, db, row, (uint16_t)(l->top + row),
                      (l->top + row) == l->cursor);
    }

    ui_render_scrollbar(c, l);
    ui_render_footer(c, v, status);
}
