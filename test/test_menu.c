/*
 * test_menu.c -- the mode shell, as it is actually drawn.
 *
 *   cc -std=c11 -Wall -Wextra -o test_menu test_menu.c \
 *      ui_render.c ui_font.c ui_chipview.c ui_list.c chipdb.c \
 *      ui_shell.c hw_caps.c chipgate.c
 *
 * The shell was compiled and tested for months and never appeared on screen,
 * because nothing rendered it. A test that only exercises shell_move() and
 * shell_select() cannot catch that. This one renders and inspects pixels.
 */

#include <stdio.h>
#include <string.h>

#include "ui_render.h"
#include "ui_shell.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-44s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static uint16_t g_fb[UI_SCREEN_W * UI_SCREEN_H];
static ui_canvas_t g_canvas = { g_fb, UI_SCREEN_W, UI_SCREEN_H };

static uint16_t px(int x, int y) { return g_fb[(size_t)y * UI_SCREEN_W + x]; }

/* Background of a menu row: column 8 sits between the cursor glyph (2..6) and
 * the label (12+), and y+10 is below the font baseline. */
static uint16_t row_bg(int row) {
    return px(8, UI_LIST_Y + row * UI_ROW_H + 10);
}

static ui_shell_t g_shell;

static const char *label(void *ctx, uint8_t i) {
    (void)ctx;
    return shell_label_at(&g_shell, i);
}

static void draw(void) {
    memset(g_fb, 0, sizeof g_fb);
    ui_render_menu(&g_canvas, "gbagate", label, NULL,
                   shell_item_count(&g_shell), shell_cursor(&g_shell), "gate");
}

static void test_every_mode_is_reachable(void) {
    printf("every mode is offered\n");

    /* A fully equipped board must show all of them, self test included. */
    shell_init(&g_shell, HW_CAP_DISPLAY | HW_CAP_LINK_PORT | HW_CAP_USB |
                         HW_CAP_WIFI | HW_CAP_CHIP_READER);
    /* Every mode, with the two link modes behind one Link entry. */
    check("all modes offered", shell_item_count(&g_shell), APP_MODE_COUNT - 1);

    /* USB Storage must be there on a board with USB: without a way to hand the
     * volume over, the only way to get files onto it is to guess what the host
     * is doing. */
    bool expose = false;
    for (uint8_t i = 0; i < shell_item_count(&g_shell); i++) {
        if (strcmp(shell_label_at(&g_shell, i), "USB Storage") == 0) {
            expose = true;
        }
    }
    check("usb storage present", expose, 1);

    /*
     * Infrared. ir_cdz.c had the protocol and no menu entry, so the feature
     * was compiled into the full image and unreachable -- which looks exactly
     * like it was never implemented.
     */
    bool ir = false;
    for (uint8_t i = 0; i < shell_item_count(&g_shell); i++) {
        if (strcmp(shell_label_at(&g_shell, i), "Infrared") == 0) {
            ir = true;
        }
    }
    check("infrared present", ir, 1);

    bool seen[APP_MODE_COUNT];
    memset(seen, 0, sizeof seen);
    for (uint8_t i = 0; i < shell_item_count(&g_shell); i++) {
        const char *l = shell_label_at(&g_shell, i);
        if (!l || !*l) {
            printf("  FAIL entry %u has no label\n", i);
            g_fail++;
        }
    }

    /* The self-test mode must be there: it is the one you reach for when you
     * do not know what else works. */
    bool has_selftest = false;
    for (uint8_t i = 0; i < shell_item_count(&g_shell); i++) {
        if (strcmp(shell_label_at(&g_shell, i), "Self Test") == 0) {
            has_selftest = true;
        }
    }
    check("self test present", has_selftest, 1);
    (void)seen;
}

/*
 * The caps mask the firmware can actually produce, not an idealised one.
 * app_main sets DISPLAY, LINK_PORT, USB, WIFI and CHIP_READER; a QEMU image
 * gets the first four. Nothing here can call app_main, but it can assert that
 * the modes worth having survive that mask -- which is what silently failed
 * when HW_CAP_LINK_PORT was never set and the menu collapsed to two entries.
 */
static void test_realistic_caps(void) {
    printf("the caps a real boot produces\n");

    /* A QEMU gate image: display, link source, wifi. No USB, no reader. */
    shell_init(&g_shell, HW_CAP_DISPLAY | HW_CAP_LINK_PORT | HW_CAP_WIFI);

    bool gate = false, selftest = false, wireless = false, diag = false;
    for (uint8_t i = 0; i < shell_item_count(&g_shell); i++) {
        const char *l = shell_label_at(&g_shell, i);
        if (!strcmp(l, "Battle Chip"))    gate = true;
        if (!strcmp(l, "Self Test"))      selftest = true;
        if (!strcmp(l, "Link"))           wireless = true;
        if (!strcmp(l, "Diagnostics"))    diag = true;
    }
    check("chip gate offered", gate, 1);
    check("self test offered", selftest, 1);
    check("link offered", wireless, 1);
    /* WiFi but no USB: Link is the wireless cable, with no choice to make. */
    for (uint8_t i = 0; i < shell_item_count(&g_shell); i++) {
        if (!strcmp(shell_label_at(&g_shell, i), "Link")) {
            g_shell.mode_cursor = i;
        }
    }
    check("one interface: straight in", shell_select(&g_shell), 1);
    check("and it is wireless", g_shell.chosen_mode, APP_MODE_LINK_WIRELESS);
    shell_back(&g_shell);
    check("diagnostics offered", diag, 1);

    /* Six of eight: everything but USB and the reader. A two-entry menu means
     * a capability is not being set. */
    check("most modes survive", shell_item_count(&g_shell) >= 6, 1);

    /* Without a link source, the link modes must disappear -- but Settings and
     * Diagnostics must not, or there is no way to find out why. */
    shell_init(&g_shell, HW_CAP_DISPLAY);
    for (uint8_t i = 0; i < shell_item_count(&g_shell); i++) {
        if (!strcmp(shell_label_at(&g_shell, i), "Self Test")) {
            check("self test hidden without a link source", 0, 1);
        }
    }
    check("settings and diagnostics remain", shell_item_count(&g_shell) >= 2, 1);
}

static void test_it_actually_draws(void) {
    printf("the menu reaches the framebuffer\n");

    shell_init(&g_shell, HW_CAP_DISPLAY | HW_CAP_LINK_PORT);
    draw();

    check("header painted", px(64, 4), COL_HEADER);
    check("footer painted", px(64, UI_SCREEN_H - 4) != 0, 1);
    check("row 0 selected", row_bg(0), COL_HEADER);
    check("row 1 not selected", row_bg(1) != COL_HEADER, 1);

    /* Ink somewhere in the label column, or the labels are not being drawn. */
    long ink = 0;
    for (int y = UI_LIST_Y; y < UI_LIST_Y + UI_ROW_H; y++) {
        for (int x = 12; x < 100; x++) {
            if (px(x, y) != COL_HEADER) ink++;
        }
    }
    check("the label is drawn", ink > 0, 1);
}

static void test_cursor_follows(void) {
    printf("cursor and scrolling\n");

    shell_init(&g_shell, HW_CAP_DISPLAY | HW_CAP_LINK_PORT | HW_CAP_USB |
                         HW_CAP_WIFI | HW_CAP_CHIP_READER);
    shell_move(&g_shell, 2);
    draw();
    check("selection moved to row 2", row_bg(2), COL_HEADER);
    check("row 0 released", row_bg(0) != COL_HEADER, 1);

}

/* A synthetic list, so the scrolling path is exercised regardless of how many
 * modes happen to exist. Today there are exactly eight modes and eight rows,
 * so the mode menu alone would never reach this code -- and the ninth mode
 * someone adds would find it untested. */
static const char *many_label(void *ctx, uint8_t i) {
    static char buf[16];
    uint8_t n = *(uint8_t *)ctx;
    if (i >= n) {
        return NULL;
    }
    snprintf(buf, sizeof buf, "Entry %u", i);
    return buf;
}

static void test_scrolling(void) {
    printf("scrolling a menu longer than the screen\n");

    uint8_t n = 20;

    /* Cursor at the top: the window starts at the top. */
    memset(g_fb, 0, sizeof g_fb);
    ui_render_menu(&g_canvas, "many", many_label, &n, n, 0, NULL);
    check("first row selected", row_bg(0), COL_HEADER);

    /* Cursor within the first screenful: still no scroll. */
    memset(g_fb, 0, sizeof g_fb);
    ui_render_menu(&g_canvas, "many", many_label, &n, n, UI_ROWS - 1, NULL);
    check("last visible row selected", row_bg(UI_ROWS - 1), COL_HEADER);

    /* Past it: the window follows, and the selection stays on screen. */
    memset(g_fb, 0, sizeof g_fb);
    ui_render_menu(&g_canvas, "many", many_label, &n, n, n - 1, NULL);
    check("window scrolled with the cursor", row_bg(UI_ROWS - 1), COL_HEADER);
    bool only_one = true;
    int selected = 0;
    for (int r = 0; r < UI_ROWS; r++) {
        if (row_bg(r) == COL_HEADER) selected++;
    }
    if (selected != 1) only_one = false;
    check("exactly one row highlighted", only_one, 1);

    /* Every entry must be reachable: walk the whole list and confirm the
     * selection is on screen at each step. */
    int unreachable = 0;
    for (uint8_t c = 0; c < n; c++) {
        memset(g_fb, 0, sizeof g_fb);
        ui_render_menu(&g_canvas, "many", many_label, &n, n, c, NULL);
        bool visible = false;
        for (int r = 0; r < UI_ROWS; r++) {
            if (row_bg(r) == COL_HEADER) visible = true;
        }
        if (!visible) unreachable++;
    }
    check("every entry reachable", unreachable, 0);
}
static void test_game_screen(void) {
    printf("game chooser\n");

    shell_init(&g_shell, HW_CAP_DISPLAY | HW_CAP_LINK_PORT);
    /* Chip gate is first, and it needs a game. */
    check("does not start immediately", shell_select(&g_shell), 0);
    check("moved to the game list", g_shell.screen, SHELL_SCREEN_GAMES);

    draw();
    check("game list drawn", row_bg(0), COL_HEADER);
    check("four games", shell_item_count(&g_shell), CG_GAME_COUNT);

    check("selecting a game starts the mode", shell_select(&g_shell), 1);
    check("running", g_shell.screen, SHELL_SCREEN_RUNNING);
}

static void test_blank_rows(void) {
    printf("short menus\n");

    /* Diagnostics-only board: far fewer entries than rows. The remainder must
     * be painted, not left as whatever was on screen before. */
    shell_init(&g_shell, 0);
    draw();
    check("at least diagnostics", shell_item_count(&g_shell) >= 1, 1);
    for (int r = shell_item_count(&g_shell); r < UI_ROWS; r++) {
        if (row_bg(r) != COL_BG) {
            printf("  FAIL row %d left unpainted\n", r);
            g_fail++;
            break;
        }
    }
    check("blank rows painted", 1, 1);
}

/* Find a menu row by label; -1 if absent. */
static int row_of(const char *label) {
    for (uint8_t i = 0; i < shell_item_count(&g_shell); i++) {
        if (!strcmp(shell_label_at(&g_shell, i), label)) {
            return i;
        }
    }
    return -1;
}

/*
 * One Link entry; the interface is chosen inside it. Only the interfaces the
 * board has are offered, and with one there is nothing to choose. Back from a
 * running link lands on the choice, the way Back from a gate lands on the
 * game list.
 */
static void test_link_chooser(void) {
    printf("link chooser\n");
    const uint32_t full = HW_CAP_DISPLAY | HW_CAP_LINK_PORT | HW_CAP_USB | HW_CAP_WIFI;
    shell_init(&g_shell, full);
    check("no Link: USB in the menu", row_of("Link: USB"), -1);
    check("no Link: Wireless in the menu", row_of("Link: Wireless"), -1);
    int link = row_of("Link");
    check("Link is second", link, 1);

    g_shell.mode_cursor = (uint8_t)link;
    check("two interfaces: a choice", shell_select(&g_shell), 0);
    check("on the link screen", g_shell.screen, SHELL_SCREEN_LINK);
    check("titled", strcmp(shell_screen_title(&g_shell), "Link over"), 0);
    check("two offered", shell_item_count(&g_shell), 2);
    check("USB first", strncmp(shell_label_at(&g_shell, 0), "USB", 3), 0);
    check("then wireless", strncmp(shell_label_at(&g_shell, 1), "Wireless", 8), 0);
    check("past the end", shell_label_at(&g_shell, 2) == NULL, 1);

    shell_move(&g_shell, 1);
    check("cursor moves", shell_cursor(&g_shell), 1);
    shell_move(&g_shell, 1);
    check("and wraps", shell_cursor(&g_shell), 0);
    shell_move(&g_shell, -1);
    check("both ways", shell_cursor(&g_shell), 1);
    check("choosing starts it", shell_select(&g_shell), 1);
    check("wireless chosen", g_shell.chosen_mode, APP_MODE_LINK_WIRELESS);
    check("named for its title", strcmp(shell_chosen_name(&g_shell), "Link: Wireless"), 0);
    check("no game", g_shell.has_game, 0);

    check("back", shell_back(&g_shell), 1);
    check("lands on the choice", g_shell.screen, SHELL_SCREEN_LINK);
    check("cursor kept", shell_cursor(&g_shell), 1);
    shell_move(&g_shell, -1);
    shell_select(&g_shell);
    check("USB chosen", g_shell.chosen_mode, APP_MODE_LINK_USB);
    shell_back(&g_shell);
    check("back again", shell_back(&g_shell), 1);
    check("at the menu", g_shell.screen, SHELL_SCREEN_MODES);
    check("on Link", g_shell.mode_cursor, link);
    check("and no further", shell_back(&g_shell), 0);

    /* USB only. */
    shell_init(&g_shell, HW_CAP_DISPLAY | HW_CAP_LINK_PORT | HW_CAP_USB);
    g_shell.mode_cursor = (uint8_t)row_of("Link");
    check("USB only: straight in", shell_select(&g_shell), 1);
    check("USB", g_shell.chosen_mode, APP_MODE_LINK_USB);
    shell_back(&g_shell);
    check("back skips the choice", g_shell.screen, SHELL_SCREEN_MODES);

    /* Neither: no Link at all, rather than a Link with nothing behind it. */
    shell_init(&g_shell, HW_CAP_DISPLAY | HW_CAP_LINK_PORT);
    check("no interface, no Link", row_of("Link"), -1);
    /* An interface but no link port: no Link either. */
    shell_init(&g_shell, HW_CAP_DISPLAY | HW_CAP_USB | HW_CAP_WIFI);
    check("no link port, no Link", row_of("Link"), -1);
}

/* Applets join the menu between the built-in modes and Settings. */
static void test_applet_entries(void) {
    printf("applet entries\n");
    const uint32_t caps = HW_CAP_DISPLAY | HW_CAP_LINK_PORT;
    shell_init(&g_shell, caps);
    uint8_t before = shell_item_count(&g_shell);
    app_mode_t a = (app_mode_t)APP_MODE_APPLET_FIRST, b = (app_mode_t)(APP_MODE_APPLET_FIRST + 1);
    check("added", shell_add_entry(&g_shell, a, "Toy", HW_CAP_DISPLAY, NULL, false), 1);
    check("listed", shell_item_count(&g_shell), before + 1);
    int settings = row_of("Settings");
    check("just before Settings", row_of("Toy"), settings - 1);
    check("Diagnostics stays last", row_of("Diagnostics"), shell_item_count(&g_shell) - 1);
    check("same mode twice refused",
          shell_add_entry(&g_shell, a, "Toy2", HW_CAP_DISPLAY, NULL, false), 0);
    check("not an applet's number refused",
          shell_add_entry(&g_shell, APP_MODE_SETTINGS, "X", 0, NULL, false), 0);
    check("NULL label refused", shell_add_entry(&g_shell, b, NULL, 0, NULL, false), 0);
    check("hidden when its hardware is absent",
          shell_add_entry(&g_shell, b, "Radio", HW_CAP_NRF24, "no nRF24", false), 1);
    check("so not listed", row_of("Radio"), -1);

    /* Selecting one runs it; one that needs a game asks first. */
    g_shell.mode_cursor = (uint8_t)row_of("Toy");
    check("runs", shell_select(&g_shell), 1);
    check("its mode", g_shell.chosen_mode, a);
    check("its name", strcmp(shell_chosen_name(&g_shell), "Toy"), 0);
    shell_back(&g_shell);
    check("back to the menu", g_shell.screen, SHELL_SCREEN_MODES);

    shell_init(&g_shell, caps);
    shell_add_entry(&g_shell, a, "Dice", HW_CAP_DISPLAY, NULL, true);
    g_shell.mode_cursor = (uint8_t)row_of("Dice");
    check("needs a game: asks", shell_select(&g_shell), 0);
    check("game list", g_shell.screen, SHELL_SCREEN_GAMES);
    check("then runs", shell_select(&g_shell), 1);
    check("with a game", g_shell.has_game, 1);
    shell_back(&g_shell);
    check("back lands on the games", g_shell.screen, SHELL_SCREEN_GAMES);

    /* A full menu refuses the next one rather than overrunning. */
    shell_init(&g_shell, caps);
    int added = 0;
    for (int i = 0; i < SHELL_EXTRA_MAX + 2; i++) {
        added += shell_add_entry(&g_shell, (app_mode_t)(APP_MODE_APPLET_FIRST + (i % SHELL_EXTRA_MAX)),
                                 "x", 0, NULL, false);
    }
    check("at most SHELL_EXTRA_MAX", added, SHELL_EXTRA_MAX);
}

int main(void) {
    test_every_mode_is_reachable();
    test_realistic_caps();
    test_it_actually_draws();
    test_cursor_follows();
    test_scrolling();
    test_game_screen();
    test_blank_rows();
    test_link_chooser();
    test_applet_entries();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
