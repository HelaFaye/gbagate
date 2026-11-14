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

/* A row's right column: a word, not the reason. */
static const char *short_fault(rom_status_t s) {
    switch (s) {
    case ROM_BAD_SIZE:     return "size!";
    case ROM_BAD_HEADER:   return "not GBA";
    case ROM_BAD_CHECKSUM: return "header!";
    case ROM_BAD_LOGO:     return "logo!";
    case ROM_UNREADABLE:   return "unread";
    default:               return "?";
    }
}

/*
 * The reason in full, for the footer. Twenty glyphs: the footer's width.
 * "wrong size" tells someone their 8 MB retail dump will never work; a
 * greyed row tells them nothing and they try again.
 */
static const char *fault_advice(rom_status_t s) {
    switch (s) {
    case ROM_BAD_SIZE:     return "over 256K or tiny";
    case ROM_BAD_HEADER:   return "not a GBA program";
    case ROM_BAD_CHECKSUM: return "header checksum bad";
    case ROM_BAD_LOGO:     return "logo bad: fix on PC";
    case ROM_UNREADABLE:   return "can't read the file";
    default:               return "can't send this";
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

    /*
     * The name, then the size or what is wrong with it, in two columns: the
     * right one is kept whole and the name shortened, since the size or the
     * fault is what decides whether to send it. The fault in full is the
     * footer's, when the row is selected.
     */
    ui_two_col(v->label, sizeof v->label, e->name,
               e->status == ROM_OK ? size : short_fault(e->status), UI_MENU_COLS);
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
        return "the PC has the drive. claim it in USB Storage";
    case ROMVIEW_NO_STORAGE:
        return "no drive found";
    case ROMVIEW_EMPTY:
    default:
        return "no programs yet. lend the drive to a PC and copy .mb files on";
    }
}

const char *romview_hint(romview_t *v, const ui_keys_t *keys) {
    const rom_entry_t *e = romview_current(v);
    if (!e) {
        return "";
    }
    if (e->status != ROM_OK) {
        return fault_advice(e->status);
    }
    char send[24];
    ui_key_verb(send, sizeof send, keys ? keys->confirm : NULL, "send");
    char back[24];
    ui_key_verb(back, sizeof back, keys ? keys->back : NULL, "back");
    ui_footer_join(v->hint, sizeof v->hint, send, back);
    return v->hint;
}
