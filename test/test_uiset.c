/*
 * test_uiset.c -- the settings editor.
 *
 *   cc -std=c11 -Wall -Wextra -o test_uiset test_uiset.c ui_settings.c settings.c chipgate.c
 *
 * One encoder does two jobs here -- move the cursor, or change a value -- so
 * the tests are mostly about that split being unambiguous. A knob that does
 * something different depending on invisible state is the failure mode.
 */

#include <stdio.h>
#include <string.h>

#include "ui_settings.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-46s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static bool has(const char *hay, const char *needle) {
    return hay && strstr(hay, needle) != NULL;
}

static void test_navigation(void) {
    printf("moving between fields\n");

    settings_t s;
    settings_defaults(&s);
    ui_settings_t v;
    uiset_init(&v, &s);

    check("every field listed", uiset_count(), SETFIELD_COUNT);
    check("starts at the top", v.cursor, 0);
    check("not editing", v.editing, 0);

    uiset_move(&v, 2);
    check("cursor moved", v.cursor, 2);

    /* Wraps, because the encoder does and there is no other way back. */
    uiset_move(&v, -3);
    check("wraps backwards", v.cursor, SETFIELD_COUNT - 1);
    uiset_move(&v, 1);
    check("wraps forwards", v.cursor, 0);

    /* Moving the cursor must not change any value. */
    check("nothing edited", uiset_dirty(&v), 0);
    check("colstart untouched", s.colstart, 2);
}

static void test_edit_mode(void) {
    printf("editing a value\n");

    settings_t s;
    settings_defaults(&s);
    ui_settings_t v;
    uiset_init(&v, &s);

    /* Row offset: the one that cannot be guessed. */
    uiset_move(&v, SETFIELD_ROWSTART);
    check("on row offset", v.cursor, SETFIELD_ROWSTART);

    check("button enters edit mode", uiset_toggle_edit(&v), 1);
    uiset_move(&v, 1);
    check("value changed", s.rowstart, 4);
    check("cursor did not move", v.cursor, SETFIELD_ROWSTART);
    check("marked dirty", uiset_dirty(&v), 1);
    check("display needs reprogramming", uiset_display_affected(&v), 1);

    check("button leaves edit mode", uiset_toggle_edit(&v), 0);
    uiset_move(&v, 1);
    check("now the cursor moves again", v.cursor, SETFIELD_ROWSTART + 1);
    check("and the value is untouched", s.rowstart, 4);
}

static void test_which_mode_is_visible(void) {
    printf("the mode is visible on screen\n");

    settings_t s;
    settings_defaults(&s);
    ui_settings_t v;
    uiset_init(&v, &s);
    uiset_move(&v, SETFIELD_COLOUR);

    const char *idle = uiset_label(&v, SETFIELD_COLOUR);
    check("value shown plainly when idle", has(idle, "[") ? 0 : 1, 1);

    uiset_toggle_edit(&v);
    const char *edit = uiset_label(&v, SETFIELD_COLOUR);
    check("bracketed while editing", has(edit, "[BGR]"), 1);

    /* Only the selected row is bracketed, or the cue means nothing. */
    check("other rows unbracketed",
          has(uiset_label(&v, SETFIELD_ACCEL), "[") ? 0 : 1, 1);

    ui_keys_t enc = ui_keys_for(0), ab = ui_keys_for(1);
    check("footer says what turning does",
          strcmp(uiset_footer(&v, &enc), "turn: set\tclick done"), 0);
    check("and on buttons", strcmp(uiset_footer(&v, &ab), "up/dn: set\tA done"), 0);
    uiset_toggle_edit(&v);
    check("and says it changed", strcmp(uiset_footer(&v, &enc), "click edit\thold back"), 0);
    check("NULL keys: the encoder's", strcmp(uiset_footer(&v, NULL), "click edit\thold back"), 0);

    /* Every row at the menu's width, its value whole at the right. */
    for (uint8_t i = 0; i < uiset_count(); i++) {
        const char *row = uiset_label(&v, i);
        check("row is 18 glyphs", (long)strlen(row), UI_MENU_COLS);
    }
    check("game value short", has(uiset_label(&v, SETFIELD_GAME), "BN6"), 1);
    check("air link value short", has(uiset_label(&v, SETFIELD_AIR), "Gen 3"), 1);
}

static void test_clamping_and_wrapping(void) {
    printf("limits\n");

    settings_t s;
    settings_defaults(&s);
    ui_settings_t v;
    uiset_init(&v, &s);
    uiset_toggle_edit(&v);

    /*
     * Offsets clamp. Wrapping from 8 to 0 while nudging looks like the knob
     * slipped, and the user has no way to tell that from a fault.
     */
    v.cursor = SETFIELD_COLSTART;
    for (int i = 0; i < 20; i++) {
        uiset_move(&v, 1);
    }
    check("colstart clamps at the top", s.colstart, 8);
    for (int i = 0; i < 30; i++) {
        uiset_move(&v, -1);
    }
    check("colstart clamps at the bottom", s.colstart, 0);

    v.cursor = SETFIELD_ACCEL;
    for (int i = 0; i < 60; i++) {
        uiset_move(&v, 1);
    }
    check("accel clamps", s.accel, 40);
    for (int i = 0; i < 80; i++) {
        uiset_move(&v, -1);
    }
    check("accel never reaches zero", s.accel, 1);

    /* Toggles and the game list wrap: there is nowhere else to go. */
    v.cursor = SETFIELD_WRAP;
    bool was = s.wrap;
    uiset_move(&v, 1);
    check("toggle flips", s.wrap, !was);
    uiset_move(&v, 1);
    check("and flips back", s.wrap, was);

    /* Input mode wraps too: three values, and someone who picked the wrong
     * one has to be able to get back. */
    v.cursor = SETFIELD_INPUT;
    s.input_mode = INPUT_MODE_COUNT - 1;
    uiset_move(&v, 1);
    check("input mode wraps", s.input_mode, 0);
    uiset_move(&v, -1);
    check("and back", s.input_mode, INPUT_MODE_COUNT - 1);

    v.cursor = SETFIELD_GAME;
    s.game = CG_GAME_COUNT - 1;
    uiset_move(&v, 1);
    check("game wraps to the first", s.game, 0);
    uiset_move(&v, -1);
    check("and back to the last", s.game, CG_GAME_COUNT - 1);
}

static void test_acceleration_ignored(void) {
    printf("acceleration does not apply to values\n");

    /*
     * The encoder accelerates so 400 chips are crossable in a few turns. Applied
     * to a 0..8 offset, a single flick would move it the whole range and back,
     * and the setting would be unsettable.
     */
    settings_t s;
    settings_defaults(&s);
    ui_settings_t v;
    uiset_init(&v, &s);
    v.cursor = SETFIELD_ROWSTART;
    uiset_toggle_edit(&v);

    uint8_t before = s.rowstart;
    uiset_move(&v, 10);
    check("a fast turn moves one step", s.rowstart, before + 1);
    uiset_move(&v, -25);
    check("and back one", s.rowstart, before);

    /* Outside edit mode the same delta should move the cursor by that much. */
    uiset_toggle_edit(&v);
    v.cursor = 0;
    uiset_move(&v, 3);
    check("cursor honours the full delta", v.cursor, 3);
}

static void test_round_trip_with_storage(void) {
    printf("edits survive a save and load\n");

    settings_t s;
    settings_defaults(&s);
    ui_settings_t v;
    uiset_init(&v, &s);
    uiset_toggle_edit(&v);

    v.cursor = SETFIELD_ROWSTART;  uiset_move(&v, 1);
    v.cursor = SETFIELD_COLSTART;  uiset_move(&v, -1);
    v.cursor = SETFIELD_COLOUR;    uiset_move(&v, 1);
    v.cursor = SETFIELD_GAME;      uiset_move(&v, 1);

    uint8_t buf[SETTINGS_BLOB_MAX + 16];
    size_t len = settings_pack(&s, buf, sizeof buf);
    check("packed", len > 0, 1);

    settings_t back;
    check("unpacked", settings_unpack(&back, buf, len), 1);
    check("rowstart", back.rowstart, s.rowstart);
    check("colstart", back.colstart, s.colstart);
    check("colour", back.bgr, s.bgr);
    check("game", back.game, s.game);
}

/* The theme row: first, so it is the first thing a new owner finds; wraps
 * both ways; names the palette; and counts as a display change, which is what
 * makes the device repaint in it at once. */
static void test_theme_row(void) {
    printf("theme\n");
    settings_t s;
    settings_defaults(&s);
    ui_settings_t v;
    uiset_init(&v, &s);
    check("first row is the theme", strncmp(uiset_label(&v, 0), "theme", 5), 0);
    check("shows the palette", strstr(uiset_label(&v, 0), "Classic") != NULL, 1);
    uiset_toggle_edit(&v);
    uiset_move(&v, 1);
    check("next palette", s.theme, UI_THEME_NAVI);
    check("named", strstr(uiset_label(&v, 0), "Navi") != NULL, 1);
    check("is a display change", uiset_display_affected(&v), 1);
    check("is saved", uiset_dirty(&v), 1);
    uiset_move(&v, -1);
    uiset_move(&v, -1);
    check("wraps backwards", s.theme, UI_THEME_COUNT - 1);
    uiset_move(&v, 1);
    check("and forwards", s.theme, UI_THEME_CLASSIC);
}

int main(void) {
    test_navigation();
    test_edit_mode();
    test_which_mode_is_visible();
    test_clamping_and_wrapping();
    test_acceleration_ignored();
    test_round_trip_with_storage();
    test_theme_row();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
