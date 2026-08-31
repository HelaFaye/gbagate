/*
 * ui_romview.c -- see ui_romview.h
 */

#include <stdio.h>
#include <string.h>

#include "ui_romview.h"

void romview_init(romview_t *v) {
    memset(v, 0, sizeof *v);
    v->state = ROMVIEW_NO_STORAGE;
}

void romview_set(romview_t *v, const rom_catalog_t *cat, romview_state_t state) {
    v->cat   = cat;
    v->state = state;
    if (state == ROMVIEW_OK && cat && cat->count == 0) {
        v->state = ROMVIEW_EMPTY;
    }
    if (!cat || v->cursor >= cat->count) {
        v->cursor = 0;
    }
}

uint8_t romview_count(const romview_t *v) {
    if (v->state != ROMVIEW_OK || !v->cat) {
        return 0;
    }
    return (uint8_t)(v->cat->count > 255 ? 255 : v->cat->count);
}

/* Sizes matter here -- a 256 KB image and a 2 KB one behave differently -- but
 * a byte count is noise at this width. */
static void human_size(uint32_t bytes, char *out, size_t cap) {
    if (bytes >= 1024u * 1024u) {
        snprintf(out, cap, "%luM", (unsigned long)(bytes / (1024u * 1024u)));
    } else if (bytes >= 1024u) {
        snprintf(out, cap, "%luK", (unsigned long)(bytes / 1024u));
    } else {
        snprintf(out, cap, "%luB", (unsigned long)bytes);
    }
}

const char *romview_label(void *ctx, uint8_t index) {
    romview_t *v = ctx;
    if (index >= romview_count(v)) {
        return NULL;
    }
    const rom_entry_t *e = &v->cat->entries[index];

    char size[8];
    human_size(e->size, size, sizeof size);

    if (e->status == ROM_OK) {
        snprintf(v->label, sizeof v->label, "%s  %s", e->name, size);
    } else {
        /*
         * The reason, not just a mark. "wrong size" tells someone their 8 MB
         * retail dump will never work; a greyed row tells them nothing and
         * they try again.
         */
        snprintf(v->label, sizeof v->label, "%s  %s", e->name,
                 rom_status_name(e->status));
    }
    return v->label;
}

bool romview_selectable(const romview_t *v, uint16_t index) {
    if (v->state != ROMVIEW_OK || !v->cat || index >= v->cat->count) {
        return false;
    }
    return v->cat->entries[index].status == ROM_OK;
}

const rom_entry_t *romview_current(const romview_t *v) {
    if (v->state != ROMVIEW_OK || !v->cat || v->cursor >= v->cat->count) {
        return NULL;
    }
    return &v->cat->entries[v->cursor];
}

void romview_move(romview_t *v, int32_t delta) {
    uint8_t n = romview_count(v);
    if (n == 0 || delta == 0) {
        return;
    }
    int32_t pos = ((int32_t)v->cursor + delta) % n;
    if (pos < 0) {
        pos += n;
    }
    v->cursor = (uint16_t)pos;
}

const char *romview_empty_text(const romview_t *v) {
    if (romview_count(v) > 0) {
        return NULL;
    }
    switch (v->state) {
    case ROMVIEW_HOST_OWNS:
        /* Not a fault. The volume has one owner at a time and the PC has it;
         * saying "no programs" here would send someone hunting a bug. */
        return "connected to PC";
    case ROMVIEW_NO_STORAGE:
        return "no storage";
    case ROMVIEW_EMPTY:
    default:
        return "drop .mb files on";
    }
}

const char *romview_hint(const romview_t *v) {
    const rom_entry_t *e = romview_current(v);
    if (!e) {
        return romview_empty_text(v);
    }
    if (e->status != ROM_OK) {
        return "cannot send this";
    }
    return "enter: send";
}
