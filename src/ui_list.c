/*
 * ui_list.c -- see ui_list.h
 */

#include <string.h>

#include "ui_list.h"

static void clamp_window(ui_list_t *l) {
    if (l->count == 0) {
        l->top = 0;
        l->cursor = 0;
        return;
    }
    if (l->cursor >= l->count) {
        l->cursor = (uint16_t)(l->count - 1);
    }

    if (l->count <= l->rows) {
        l->top = 0;
        return;
    }

    if (l->cursor < l->top) {
        l->top = l->cursor;
    } else if (l->cursor >= (uint16_t)(l->top + l->rows)) {
        l->top = (uint16_t)(l->cursor - l->rows + 1);
    }

    uint16_t max_top = (uint16_t)(l->count - l->rows);
    if (l->top > max_top) {
        l->top = max_top;
    }
}

void ui_list_init(ui_list_t *l, uint8_t rows, bool wrap) {
    memset(l, 0, sizeof *l);
    l->rows  = rows ? rows : UI_ROWS;
    l->wrap  = wrap;
    l->dirty = true;
}

void ui_list_set(ui_list_t *l, const uint16_t *items, uint16_t count) {
    l->items = items;
    l->count = count;
    if (l->cursor >= count) {
        l->cursor = count ? (uint16_t)(count - 1) : 0;
    }
    l->top = 0;
    clamp_window(l);
    l->dirty = true;
}

void ui_list_move(ui_list_t *l, int32_t delta) {
    if (l->count == 0 || delta == 0) {
        return;
    }

    int32_t n = (int32_t)l->count;
    int32_t pos = (int32_t)l->cursor + delta;

    if (l->wrap) {
        pos %= n;
        if (pos < 0) {
            pos += n;
        }
    } else {
        if (pos < 0) {
            pos = 0;
        }
        if (pos >= n) {
            pos = n - 1;
        }
    }

    if ((uint16_t)pos != l->cursor) {
        l->cursor = (uint16_t)pos;
        l->dirty  = true;
    }
    clamp_window(l);
}

void ui_list_page(ui_list_t *l, int32_t pages) {
    ui_list_move(l, pages * (int32_t)l->rows);
}

void ui_list_goto(ui_list_t *l, uint16_t position) {
    if (l->count == 0) {
        return;
    }
    if (position >= l->count) {
        position = (uint16_t)(l->count - 1);
    }
    if (position != l->cursor) {
        l->cursor = position;
        l->dirty  = true;
    }
    clamp_window(l);
}

bool ui_list_row(const ui_list_t *l, uint8_t row, uint16_t *item) {
    if (row >= l->rows) {
        return false;
    }
    uint32_t idx = (uint32_t)l->top + row;
    if (idx >= l->count) {
        return false;
    }
    if (item) {
        *item = l->items[idx];
    }
    return true;
}

bool ui_list_selected(const ui_list_t *l, uint16_t *item) {
    if (l->count == 0) {
        return false;
    }
    if (item) {
        *item = l->items[l->cursor];
    }
    return true;
}

uint8_t ui_list_cursor_row(const ui_list_t *l) {
    if (l->count == 0) {
        return 0;
    }
    return (uint8_t)(l->cursor - l->top);
}

bool ui_list_scrollbar(const ui_list_t *l, uint8_t *y, uint8_t *h) {
    if (l->count <= l->rows) {
        return false;
    }

    uint32_t track = UI_LIST_H;
    uint32_t thumb = (track * l->rows) / l->count;
    if (thumb < 4) {
        thumb = 4; /* stays grabbable at 411 entries */
    }

    uint32_t span = l->count - l->rows;
    uint32_t travel = track - thumb;
    uint32_t pos = span ? (travel * l->top) / span : 0;

    if (y) {
        *y = (uint8_t)pos;
    }
    if (h) {
        *h = (uint8_t)thumb;
    }
    return true;
}
