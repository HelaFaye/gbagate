/*
 * test_chipview.c -- chip screen behaviour and the mode shell, against the
 * real bn6g database.
 *
 *   cc -std=c11 -Wall -Wextra -o test_chipview test_chipview.c \
 *      ui_chipview.c ui_shell.c chipdb.c hw_caps.c chipgate.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_chipview.h"
#include "ui_shell.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-42s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static void check_str(const char *what, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        printf("  FAIL %-42s got \"%s\", want \"%s\"\n",
               what, got ? got : "(null)", want);
        g_fail++;
    }
}

static chipdb_t g_db;
static uint16_t g_view[512];
static void *g_blob;

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

/* ---- tabs -------------------------------------------------------------- */

static void test_tabs(void) {
    printf("library tabs\n");

    chipview_t v;
    chipview_init(&v, &g_db, g_view, 512);

    check("starts on Standrd", v.tab, CHIP_TAB_STANDARD);
    check("Standrd available", chipview_tab_available(&v, CHIP_TAB_STANDARD), 1);
    check("Mega available", chipview_tab_available(&v, CHIP_TAB_MEGA), 1);
    check("Giga available", chipview_tab_available(&v, CHIP_TAB_GIGA), 1);

    /* No favourites yet, so that tab is greyed. */
    check("favourites greyed", chipview_tab_available(&v, CHIP_TAB_FAVORITES), 0);
    check("cannot select a greyed tab", chipview_set_tab(&v, CHIP_TAB_FAVORITES), 0);
    check("still on Standrd", v.tab, CHIP_TAB_STANDARD);

    printf("  tab sizes:");
    for (int t = CHIP_TAB_STANDARD; t < CHIP_TAB_COUNT; t++) {
        if (chipview_set_tab(&v, (chip_tab_t)t)) {
            printf(" %s=%u", chipview_tab_name((chip_tab_t)t), v.view_count);
        } else {
            printf(" %s=grey", chipview_tab_name((chip_tab_t)t));
        }
    }
    printf("\n");

    /* Cycling must step over greyed tabs rather than landing on them. */
    chipview_set_tab(&v, CHIP_TAB_STANDARD);
    for (int i = 0; i < 12; i++) {
        chip_tab_t landed = chipview_tab_cycle(&v, 1);
        if (!chipview_tab_available(&v, landed)) {
            check("never lands on a greyed tab", 0, 1);
            break;
        }
    }
    for (int i = 0; i < 12; i++) {
        chip_tab_t landed = chipview_tab_cycle(&v, -1);
        if (!chipview_tab_available(&v, landed)) {
            check("backwards too", 0, 1);
            break;
        }
    }
}

static void test_sort(void) {
    printf("sorting\n");

    chipview_t v;
    chipview_init(&v, &g_db, g_view, 512);
    chipview_set_tab(&v, CHIP_TAB_STANDARD);

    chipview_set_sort(&v, CHIP_SORT_NAME);
    int bad = 0;
    for (uint16_t i = 1; i < v.view_count; i++) {
        const char *a = chipdb_name(&g_db, v.view[i - 1]);
        const char *b = chipdb_name(&g_db, v.view[i]);
        if (a && b && strcmp(a, b) > 0) bad++;
    }
    check("name order is monotonic", bad, 0);
    printf("  by name, first three: %s %s %s\n",
           chipdb_name(&g_db, v.view[0]), chipdb_name(&g_db, v.view[1]),
           chipdb_name(&g_db, v.view[2]));

    chipview_set_sort(&v, CHIP_SORT_ATTACK);
    bad = 0;
    for (uint16_t i = 1; i < v.view_count; i++) {
        chip_record_t a, b;
        chipdb_get(&g_db, v.view[i - 1], &a);
        chipdb_get(&g_db, v.view[i], &b);
        if (a.damage < b.damage) bad++;
    }
    check("attack is strongest first", bad, 0);
    {
        chip_record_t top;
        chipdb_get(&g_db, v.view[0], &top);
        printf("  strongest: %s at %u\n", chipdb_name(&g_db, v.view[0]), top.damage);
    }

    chipview_set_sort(&v, CHIP_SORT_ELEMENT);
    bad = 0;
    for (uint16_t i = 1; i < v.view_count; i++) {
        chip_record_t a, b;
        chipdb_get(&g_db, v.view[i - 1], &a);
        chipdb_get(&g_db, v.view[i], &b);
        if (a.element > b.element) bad++;
    }
    check("element groups", bad, 0);

    /* Sorting must not change what is in the tab, only its order. */
    uint16_t n_before = v.view_count;
    chipview_set_sort(&v, CHIP_SORT_LIBRARY);
    check("count is stable across sorts", v.view_count, n_before);
}

static void test_favorites(void) {
    printf("favourites\n");

    chipview_t v;
    chipview_init(&v, &g_db, g_view, 512);
    chipview_set_tab(&v, CHIP_TAB_STANDARD);

    uint16_t a = v.view[0], b = v.view[5], c = v.view[9];
    check("none yet", chipview_favorite_count(&v), 0);

    chipview_toggle_favorite(&v, a);
    chipview_toggle_favorite(&v, b);
    chipview_toggle_favorite(&v, c);
    check("three starred", chipview_favorite_count(&v), 3);
    check("is favourite", chipview_is_favorite(&v, b), 1);
    check("dirty for persistence", v.favorites_dirty, 1);

    /* The tab ungreys once something is starred. */
    check("favourites now available", chipview_tab_available(&v, CHIP_TAB_FAVORITES), 1);
    check("selectable", chipview_set_tab(&v, CHIP_TAB_FAVORITES), 1);
    check("holds three", v.view_count, 3);

    /*
     * Un-starring from inside the favourites tab has to drop the row, or the
     * list keeps showing something that is no longer a favourite.
     */
    chipview_toggle_favorite(&v, b);
    check("row dropped", v.view_count, 2);
    check("two left", chipview_favorite_count(&v), 2);

    /* Emptying the tab must move somewhere selectable, not strand the user. */
    chipview_toggle_favorite(&v, chipview_slot_at(&v, 0));
    chipview_toggle_favorite(&v, chipview_slot_at(&v, 0));
    check("no favourites left", chipview_favorite_count(&v), 0);
    check("moved off the empty tab", v.tab != CHIP_TAB_FAVORITES, 1);
    check("landed somewhere real", chipview_tab_available(&v, v.tab), 1);
}

static void test_move_mode(void) {
    printf("move mode\n");

    chipview_t v;
    chipview_init(&v, &g_db, g_view, 512);
    chipview_set_tab(&v, CHIP_TAB_STANDARD);

    uint16_t first = chipview_slot_at(&v, 0);
    uint16_t third = chipview_slot_at(&v, 2);

    /* Picking up does nothing in Slot-In mode. */
    chipview_pick(&v, 0);
    check("no pickup in slot-in mode", chipview_held(&v), CHIPVIEW_NONE);

    chipview_set_mode(&v, CHIP_MODE_MOVE);
    chipview_pick(&v, 0);
    check("held", chipview_held(&v), 0);
    check("swapped", chipview_place(&v, 2), 1);
    check("first moved", chipview_slot_at(&v, 2), first);
    check("third moved", chipview_slot_at(&v, 0), third);
    check("nothing held after", chipview_held(&v), CHIPVIEW_NONE);

    /* A hand-made order is not any of the sorts, and must be labelled so it is
     * not silently discarded on the next rebuild. */
    check("sort became custom", v.sort, CHIP_SORT_CUSTOM);
    check_str("labelled", chipview_sort_name(v.sort), "Custom");

    chipview_rebuild(&v);
    check("custom order survives rebuild", chipview_slot_at(&v, 0), third);

    /* Placing on itself cancels; the caller may treat that as a send. */
    chipview_pick(&v, 1);
    check("self-place is not a swap", chipview_place(&v, 1), 0);
    check("released", chipview_held(&v), CHIPVIEW_NONE);

    chipview_pick(&v, 1);
    chipview_cancel(&v);
    check("cancel releases", chipview_held(&v), CHIPVIEW_NONE);

    /* Switching mode must not leave something dangling in hand. */
    chipview_pick(&v, 1);
    chipview_set_mode(&v, CHIP_MODE_SLOT_IN);
    check("mode change drops the held chip", chipview_held(&v), CHIPVIEW_NONE);
}

/* ---- shell ------------------------------------------------------------- */

static void test_shell(void) {
    printf("mode shell\n");

    /* A bare board: display and link port, no USB, no WiFi, no reader. */
    ui_shell_t s;
    shell_init(&s, HW_CAP_DISPLAY | HW_CAP_LINK_PORT);
    /* Send Program, Infrared, Self Test, Settings, Diagnostics. Link needs
     * USB or WiFi, and Battle Chip is an applet now (applets/), which the
     * firmware adds after shell_init. */
    check("modes offered", shell_item_count(&s), 5);
    check_str("first is Send Program", shell_label_at(&s, 0), "Send Program");

    for (uint8_t i = 0; i < shell_item_count(&s); i++) {
        if (strcmp(shell_label_at(&s, i), "Link: USB") == 0) {
            check("USB mode hidden without USB", 0, 1);
        }
        if (strcmp(shell_label_at(&s, i), "Read Chip") == 0) {
            check("reader hidden without a reader", 0, 1);
        }
    }

    /* Diagnostics requires nothing, so it is always reachable -- the screen
     * that explains a missing part must not itself be gated on parts. */
    bool diag = false;
    for (uint8_t i = 0; i < shell_item_count(&s); i++) {
        if (strcmp(shell_label_at(&s, i), "Diagnostics") == 0) diag = true;
    }
    check("diagnostics always present", diag, 1);

    /* Full board. */
    shell_init(&s, HW_CAP_DISPLAY | HW_CAP_LINK_PORT | HW_CAP_USB |
                   HW_CAP_WIFI | HW_CAP_CHIP_READER);
    /* The two link modes share one Link entry. */
    check("everything offered", shell_item_count(&s), APP_MODE_COUNT - 1);

    /* Wrapping, for a rotary encoder. */
    shell_move(&s, -1);
    check("wraps backwards", shell_cursor(&s), APP_MODE_COUNT - 2);
    shell_move(&s, 1);
    check("wraps forwards", shell_cursor(&s), 0);

    /* Self Test needs a game, so selecting it advances rather than starting. */
    while (strcmp(shell_current_label(&s), "Self Test") != 0) {
        shell_move(&s, 1);
    }
    check("self test does not start yet", shell_select(&s), 0);
    check("on the game screen", s.screen, SHELL_SCREEN_GAMES);
    check("four games", shell_item_count(&s), CG_GAME_COUNT);

    shell_move(&s, 3);
    check("selects a game", shell_select(&s), 1);
    check("running", s.screen, SHELL_SCREEN_RUNNING);
    check("game recorded", s.has_game, 1);
    check("chose BN6 US", s.chosen_game, CG_GAME_BN6_US);

    /* Coming back lands on the game list, not the top: changing game is the
     * usual reason to leave a running mode. */
    check("back works", shell_back(&s), 1);
    check("back to games", s.screen, SHELL_SCREEN_GAMES);
    check("back again", shell_back(&s), 1);
    check("back to modes", s.screen, SHELL_SCREEN_MODES);
    check("cannot go above the top", shell_back(&s), 0);

    /* A mode with no game starts immediately. */
    while (strcmp(shell_current_label(&s), "Settings") != 0) {
        shell_move(&s, 1);
    }
    check("settings starts at once", shell_select(&s), 1);
    check("no game needed", s.has_game, 0);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "chipdb/bn6g.chdb";
    if (!load(path)) {
        printf("cannot open %s\n", path);
        return 2;
    }
    printf("using %s, %u records\n\n", path, chipdb_count(&g_db));

    test_tabs();
    test_sort();
    test_favorites();
    test_move_mode();
    test_shell();

    free(g_blob);
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
