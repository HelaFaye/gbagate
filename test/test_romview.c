/*
 * test_romview.c -- the Send Program browser.
 *
 *   cc -std=c11 -Wall -Wextra -o test_romview test_romview.c \
 *      ui_romview.c rom_catalog.c gba_multiboot.c
 *
 * The states that matter are the empty ones. A drive with nothing on it, a
 * drive a PC currently owns, and a partition that would not mount all show an
 * empty list, and telling the user the wrong reason sends them hunting a fault
 * that is not there.
 */

#include <stdio.h>
#include <string.h>

#include "ui_romview.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-44s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static void check_str(const char *what, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        printf("  FAIL %-44s got \"%s\", want \"%s\"\n",
               what, got ? got : "(null)", want);
        g_fail++;
    }
}

static bool has(const char *hay, const char *needle) {
    return hay && strstr(hay, needle) != NULL;
}

/* ---- a catalogue built by hand ------------------------------------------ */

static rom_catalog_t make_cat(void) {
    rom_catalog_t c;
    memset(&c, 0, sizeof c);

    struct { const char *name; uint32_t size; rom_status_t st; } rows[] = {
        { "demo.mb",         2048,    ROM_OK },
        { "huge.gba",        8388608, ROM_BAD_SIZE },
        { "garbage.mb",      4096,    ROM_BAD_HEADER },
        { "patched.mb",      131072,  ROM_BAD_LOGO },
        { "big_but_fine.mb", 262144,  ROM_OK },
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        strncpy(c.entries[i].name, rows[i].name, ROM_NAME_MAX - 1);
        c.entries[i].size   = rows[i].size;
        c.entries[i].status = rows[i].st;
        c.count++;
        if (rows[i].st == ROM_OK) {
            c.playable++;
        } else {
            c.rejected++;
        }
    }
    return c;
}

/* ---- tests --------------------------------------------------------------- */

static void test_listing(void) {
    printf("listing\n");

    rom_catalog_t cat = make_cat();
    romview_t v;
    romview_init(&v);
    romview_set(&v, &cat, ROMVIEW_OK);

    check("all entries listed", romview_count(&v), 5);

    /* Bad files are listed, not hidden: someone who dragged a file across
     * needs to know why it will not run. */
    printf("  rows:\n");
    for (uint8_t i = 0; i < romview_count(&v); i++) {
        printf("    %s\n", romview_label(&v, i));
    }

    check("size shown for a good rom", has(romview_label(&v, 0), "2K"), 1);
    check("256K shown as K", has(romview_label(&v, 4), "256K"), 1);
    check("8 MB shown as M or refused", has(romview_label(&v, 1), "wrong size"), 1);
    check("bad header explains itself",
          has(romview_label(&v, 2), "not a GBA image"), 1);
    check("bad logo explains itself", has(romview_label(&v, 3), "logo"), 1);
    check("past the end is NULL", romview_label(&v, 5) == NULL, 1);
}

static void test_selectability(void) {
    printf("what can be sent\n");

    rom_catalog_t cat = make_cat();
    romview_t v;
    romview_init(&v);
    romview_set(&v, &cat, ROMVIEW_OK);

    check("good rom selectable", romview_selectable(&v, 0), 1);
    check("oversize not", romview_selectable(&v, 1), 0);
    check("bad header not", romview_selectable(&v, 2), 0);
    check("bad logo not", romview_selectable(&v, 3), 0);
    check("second good one selectable", romview_selectable(&v, 4), 1);
    check("out of range", romview_selectable(&v, 99), 0);

    /* The footer must say why the button will do nothing, rather than the
     * button silently doing nothing. */
    check_str("hint on a good rom", romview_hint(&v), "enter: send");
    romview_move(&v, 1);
    check_str("hint on a bad one", romview_hint(&v), "cannot send this");
}

static void test_cursor(void) {
    printf("cursor\n");

    rom_catalog_t cat = make_cat();
    romview_t v;
    romview_init(&v);
    romview_set(&v, &cat, ROMVIEW_OK);

    check("starts at the top", v.cursor, 0);
    romview_move(&v, 2);
    check("moved", v.cursor, 2);
    romview_move(&v, -1);
    check("back", v.cursor, 1);

    /* Wraps, because the encoder does. */
    romview_move(&v, -2);
    check("wraps backwards", v.cursor, 4);
    romview_move(&v, 1);
    check("wraps forwards", v.cursor, 0);

    const rom_entry_t *e = romview_current(&v);
    check("current entry", e != NULL, 1);
    check_str("is the first", e->name, "demo.mb");
}

static void test_empty_states(void) {
    printf("the three empty states\n");

    romview_t v;
    romview_init(&v);

    /*
     * A PC holding the volume is not a fault. Saying "no programs" would send
     * someone looking for a bug that is not there -- the volume has exactly one
     * owner at a time and the PC has it.
     */
    romview_set(&v, NULL, ROMVIEW_HOST_OWNS);
    check("nothing listed", romview_count(&v), 0);
    check_str("says who has it", romview_empty_text(&v), "connected to PC");

    /* A mounted but empty drive is actionable, and the words should say so. */
    rom_catalog_t empty;
    memset(&empty, 0, sizeof empty);
    romview_set(&v, &empty, ROMVIEW_OK);
    check("empty catalogue becomes the empty state", v.state, ROMVIEW_EMPTY);
    check_str("tells the user what to do",
              romview_empty_text(&v), "drop .mb files on");

    /* A partition that would not mount is a real fault, and different again. */
    romview_set(&v, NULL, ROMVIEW_NO_STORAGE);
    check_str("distinct from the others", romview_empty_text(&v), "no storage");

    /* All three are unselectable and safe to move around in. */
    check("nothing selectable", romview_selectable(&v, 0), 0);
    check("no current entry", romview_current(&v) == NULL, 1);
    romview_move(&v, 3);
    check("moving is harmless", v.cursor, 0);
    check("no labels", romview_label(&v, 0) == NULL, 1);

    /* With entries, there is no empty text to show. */
    rom_catalog_t cat = make_cat();
    romview_set(&v, &cat, ROMVIEW_OK);
    check("no empty text when full", romview_empty_text(&v) == NULL, 1);
}

static void test_long_names(void) {
    printf("long names\n");

    /* FAT long names reach 40 characters here, and the longest status is
     * "bad logo - repair on PC". The label must hold both without cutting a
     * status mid-word. */
    rom_catalog_t cat;
    memset(&cat, 0, sizeof cat);
    memset(cat.entries[0].name, 'a', ROM_NAME_MAX - 1);
    cat.entries[0].size = 4096;
    cat.entries[0].status = ROM_BAD_LOGO;
    cat.count = 1;

    romview_t v;
    romview_init(&v);
    romview_set(&v, &cat, ROMVIEW_OK);

    const char *l = romview_label(&v, 0);
    check("label produced", l != NULL, 1);
    check("status survives intact", has(l, "repair on PC"), 1);
}

int main(void) {
    test_listing();
    test_selectability();
    test_cursor();
    test_empty_states();
    test_long_names();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
