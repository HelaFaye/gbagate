/*
 * test_applets.c -- the applet layer, and the three example applets, run the
 * way the firmware runs them: listed in the shell's menu, started and stopped
 * by the dispatcher, given buttons, drawn.
 *
 *   make run-applets            (handed the BN6 chip database, for Chip Dice)
 *
 * The examples are what doc/APPLETS.md tells people to copy, so they are held
 * to the same standard as the firmware: each behaviour the walkthrough
 * describes is asserted here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applet.h"
#include "ui_font.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-46s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

extern const applet_t applet_hello, applet_test_pattern, applet_chip_dice;

static uint16_t    g_fb[UI_SCREEN_W * UI_SCREEN_H];
static ui_canvas_t g_canvas = { g_fb, UI_SCREEN_W, UI_SCREEN_H };
static uint16_t px(int x, int y) { return g_fb[(size_t)y * UI_SCREEN_W + x]; }

static bool has_colour(int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if (px(xx, yy) == c) return true;
    return false;
}

/* ---- the database Chip Dice opens --------------------------------------- */

static void  *g_blob;
static size_t g_blob_len;
static int    g_opens;

static bool load_db(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_blob = malloc((size_t)n);
    g_blob_len = fread(g_blob, 1, (size_t)n, f);
    fclose(f);
    return g_blob_len == (size_t)n;
}

static bool open_db(cg_game_t game, chipdb_t *db) {
    g_opens++;
    return game == CG_GAME_BN6_US && chipdb_open(db, g_blob, g_blob_len);
}

static uint32_t g_seq;
static uint32_t counting_random(void) { return g_seq++ * 2654435761u; }

/* ---- the registry -------------------------------------------------------- */

static void test_registry(void) {
    printf("registry\n");
    /* applets/applets.def: test_pattern, chip_dice; hello commented out. */
    check("two listed", applet_count(), 2);
    check("Test Pattern first", applet_at(0) == &applet_test_pattern, 1);
    check("then Chip Dice", applet_at(1) == &applet_chip_dice, 1);
    check("past the end", applet_at(2) == NULL, 1);

    check("examples are valid", applet_valid(&applet_hello) &&
          applet_valid(&applet_test_pattern) && applet_valid(&applet_chip_dice), 1);
    check("NULL is not", applet_valid(NULL), 0);
    applet_t a = applet_hello;
    a.name = NULL;
    check("no name is not", applet_valid(&a), 0);
    a.name = "";
    check("an empty name is not", applet_valid(&a), 0);
    a = applet_hello;
    a.draw = applet_test_pattern.draw;
    check("page() and draw() both is not", applet_valid(&a), 0);
    a.page = NULL; a.draw = NULL;
    check("neither is not", applet_valid(&a), 0);

    check("a built-in mode is not an applet", applet_for_mode(APP_MODE_SETTINGS) == NULL, 1);
    check("an applet's mode finds it",
          applet_for_mode((app_mode_t)APP_MODE_APPLET_FIRST) == &applet_test_pattern, 1);
    check("an unused applet number finds none",
          applet_for_mode((app_mode_t)(APP_MODE_APPLET_FIRST + 5)) == NULL, 1);
    check("input to no applet is unused",
          applet_input_to(APP_MODE_SETTINGS, APPLET_IN_CONFIRM, 0), 0);
}

static void test_env(void) {
    printf("environment\n");
    applet_bind_env(NULL);
    check("a default before any is bound", applet_env() != NULL, 1);
    check("whose random is never NULL", applet_env()->random != NULL, 1);
    check("and which offers no database", applet_env()->open_db == NULL, 1);
    uint32_t r1 = applet_env()->random(), r2 = applet_env()->random();
    check("random varies", r1 != r2, 1);

    static applet_env_t env;
    memset(&env, 0, sizeof env);
    applet_bind_env(&env);
    check("binding takes it", applet_env() == &env, 1);
    check("a missing random is filled in", env.random != NULL, 1);
    applet_bind_env(NULL);
    check("NULL restores the default", applet_env() != &env, 1);
}

static void test_handlers(void) {
    printf("handlers\n");
    app_mode_handler_t h[APPLET_MAX + 2];
    check("one per applet", applet_handlers(h, APPLET_MAX + 2), 2);
    check("modes from APP_MODE_APPLET_FIRST", h[0].mode, APP_MODE_APPLET_FIRST);
    check("in order", h[1].mode, APP_MODE_APPLET_FIRST + 1);
    check("ctx is the applet", h[1].ctx == &applet_chip_dice, 1);
    check("requires carried", h[0].requires, HW_CAP_DISPLAY);
    check("needs_game carried", h[1].needs_game, 1);
    check("a short buffer is respected", applet_handlers(h, 1), 1);
    check("and an empty one", applet_handlers(h, 0), 0);
}

/* ---- end to end: shell, dispatcher, applet ------------------------------- */

typedef struct {
    ui_shell_t         shell;
    app_dispatch_t     d;
    app_mode_handler_t h[APPLET_MAX];
    applet_env_t       env;
} host_t;

static void host_init(host_t *t, uint32_t caps) {
    memset(t, 0, sizeof *t);
    shell_init(&t->shell, caps);
    applet_add_to_shell(&t->shell);
    size_t n = applet_handlers(t->h, APPLET_MAX);
    t->env.caps    = caps;
    t->env.hint    = "click: next";
    t->env.open_db = open_db;
    t->env.random  = counting_random;
    applet_bind_env(&t->env);
    app_dispatch_init(&t->d, t->h, n, caps, NULL);
}

static bool host_choose(host_t *t, const char *label) {
    for (uint8_t i = 0; i < shell_item_count(&t->shell); i++) {
        if (!strcmp(shell_label_at(&t->shell, i), label)) {
            t->shell.mode_cursor = i;
            shell_select(&t->shell);
            if (t->shell.screen == SHELL_SCREEN_GAMES) {
                /* The game list runs in cg_game_t order. */
                t->shell.game_cursor = (uint8_t)CG_GAME_BN6_US;
                shell_select(&t->shell);
            }
            return app_dispatch_from_shell(&t->d, &t->shell) == APP_DISPATCH_OK;
        }
    }
    return false;
}

static void host_draw(host_t *t) {
    memset(g_fb, 0, sizeof g_fb);
    applet_render(applet_for_mode(t->shell.chosen_mode), &t->env, &g_canvas);
}

static void test_menu_placement(void) {
    printf("menu placement\n");
    host_t t;
    host_init(&t, HW_CAP_DISPLAY | HW_CAP_LINK_PORT | HW_CAP_USB);
    int tp = -1, dice = -1, settings = -1;
    for (uint8_t i = 0; i < shell_item_count(&t.shell); i++) {
        const char *l = shell_label_at(&t.shell, i);
        if (!strcmp(l, "Test Pattern")) tp = i;
        if (!strcmp(l, "Chip Dice"))    dice = i;
        if (!strcmp(l, "Settings"))     settings = i;
    }
    check("Test Pattern listed", tp >= 0, 1);
    check("before Chip Dice", dice, tp + 1);
    check("both before Settings", settings, dice + 1);
    host_init(&t, 0);
    check("hidden without a display", shell_item_count(&t.shell), 1);  /* Diagnostics */
}

/* Test Pattern: draw() on the whole canvas, patterns changed by turning. */
static void test_test_pattern(void) {
    printf("test pattern\n");
    host_t t;
    host_init(&t, HW_CAP_DISPLAY);
    check("starts", host_choose(&t, "Test Pattern"), 1);
    check("running", app_dispatch_running(&t.d), 1);

    host_draw(&t);
    check("bars: white first", px(8, 64), RGB565(255, 255, 255));
    check("bars: red sixth", px(5 * 16 + 8, 64), RGB565(255, 0, 0));
    check("bars: blue seventh", px(6 * 16 + 8, 64), RGB565(0, 0, 255));
    check("bars: black last", px(7 * 16 + 8, 64), RGB565(0, 0, 0));

    check("turning is used", applet_input_to(t.shell.chosen_mode, APPLET_IN_MOVE, 3), 1);
    host_draw(&t);
    check("border: top-left", px(0, 0), RGB565(255, 255, 255));
    check("border: bottom-right", px(127, 127), RGB565(255, 255, 255));
    check("border: corner mark", px(5, 5), RGB565(255, 0, 0));
    check("border: inside is black", px(64, 30), RGB565(0, 0, 0));

    applet_input_to(t.shell.chosen_mode, APPLET_IN_CONFIRM, 0);
    host_draw(&t);
    check("grid: a line at 16", px(16, 40), RGB565(160, 160, 160));
    check("grid: between lines", px(20, 40), RGB565(0, 0, 0));

    applet_input_to(t.shell.chosen_mode, APPLET_IN_MOVE, 1);
    host_draw(&t);
    check("theme: the theme's background", px(100, 6), ui_theme()->bg);
    check("theme: a header swatch", has_colour(3, 12, 20, 120, ui_theme()->header), 1);

    applet_input_to(t.shell.chosen_mode, APPLET_IN_MOVE, -1);
    host_draw(&t);
    check("back a pattern: grid", px(16, 40), RGB565(160, 160, 160));
    /* One pattern per event, however far the knob went: a fast flick must
     * not skip the one you were looking for. */
    applet_input_to(t.shell.chosen_mode, APPLET_IN_MOVE, -3);
    host_draw(&t);
    check("a big turn is one step: border", px(64, 30), RGB565(0, 0, 0));
    applet_input_to(t.shell.chosen_mode, APPLET_IN_MOVE, -1);
    applet_input_to(t.shell.chosen_mode, APPLET_IN_MOVE, -1);
    host_draw(&t);
    check("and wraps past the first", px(100, 6), ui_theme()->bg);

    check("Back is not used", applet_input_to(t.shell.chosen_mode, APPLET_IN_BACK, 0), 0);
    shell_back(&t.shell);
    app_dispatch_from_shell(&t.d, &t.shell);
    check("so the host leaves", app_dispatch_running(&t.d), 0);
    check("to the menu", t.shell.screen, SHELL_SCREEN_MODES);

    /* Re-entering starts from the first pattern again. */
    host_choose(&t, "Test Pattern");
    host_draw(&t);
    check("enter() resets", px(8, 64), RGB565(255, 255, 255));
}

/* Chip Dice: needs a game, uses open_db and random, refuses without them. */
static void test_chip_dice(void) {
    printf("chip dice\n");
    host_t t;
    host_init(&t, HW_CAP_DISPLAY);
    g_opens = 0;
    check("starts with a game", host_choose(&t, "Chip Dice"), 1);
    check("asked which game", t.shell.has_game, 1);
    check("opened its database", g_opens, 1);

    host_draw(&t);
    check("header in the theme", px(UI_SCREEN_W - 2, 1), ui_theme()->header);
    check("invites a roll", has_colour(4, 40, 100, UI_FONT_H, ui_theme()->text), 1);

    g_seq = 1;
    check("confirm rolls", applet_input_to(t.shell.chosen_mode, APPLET_IN_CONFIRM, 0), 1);
    host_draw(&t);
    /* The card is drawn in the chip's element colour. */
    bool card = false;
    const uint8_t elems[] = { CHIP_ELEM_NONE, CHIP_ELEM_FIRE, CHIP_ELEM_AQUA,
                              CHIP_ELEM_ELEC, CHIP_ELEM_WOOD };
    for (unsigned i = 0; i < sizeof elems; i++) {
        card |= px(35, 23) == ui_element_colour(elems[i]);
    }
    check("a card in an element colour", card, 1);
    check("a name below it", has_colour(2, 72, UI_SCREEN_W - 4, UI_FONT_H, ui_theme()->text), 1);

    /* Same random sequence, same chip: the roll is the generator's, nothing
     * else's. */
    uint16_t first[UI_SCREEN_W * UI_SCREEN_H];
    memcpy(first, g_fb, sizeof first);
    applet_input_to(t.shell.chosen_mode, APPLET_IN_MOVE, 1);
    host_draw(&t);
    check("turning steps to another chip", memcmp(first, g_fb, sizeof first) != 0, 1);
    applet_input_to(t.shell.chosen_mode, APPLET_IN_MOVE, -1);
    host_draw(&t);
    /* "roll 1" is unchanged and so is the chip: the screen is the same. */
    check("and back to the rolled one", memcmp(first, g_fb, sizeof first), 0);

    check("Back is not used", applet_input_to(t.shell.chosen_mode, APPLET_IN_BACK, 0), 0);
    shell_back(&t.shell);
    app_dispatch_from_shell(&t.d, &t.shell);
    check("leaves to the game list", t.shell.screen, SHELL_SCREEN_GAMES);

    /* A game with no database: refused, and the shell says so. */
    t.shell.game_cursor = (uint8_t)CG_GAME_BN4;
    shell_select(&t.shell);
    check("no database: refused", app_dispatch_from_shell(&t.d, &t.shell), APP_DISPATCH_REFUSED);

    /* An image with no databases at all. */
    host_init(&t, HW_CAP_DISPLAY);
    t.env.open_db = NULL;
    check("no open_db: refused", host_choose(&t, "Chip Dice"), 0);
    check("with the reason", t.d.last_status, APP_DISPATCH_REFUSED);
}

/* Hello: the walkthrough's page applet, not in the menu. */
static void test_hello(void) {
    printf("hello\n");
    applet_env_t env = { .hint = "hold back" };
    applet_bind_env(&env);
    check("enters", applet_hello.enter(&env), 1);
    check("confirm used", applet_hello.input(&env, APPLET_IN_CONFIRM, 0), 1);
    applet_hello.input(&env, APPLET_IN_CONFIRM, 0);
    check("turn used", applet_hello.input(&env, APPLET_IN_MOVE, -2), 1);
    check("Back not used", applet_hello.input(&env, APPLET_IN_BACK, 0), 0);

    ui_page_t p;
    ui_page_init(&p, applet_hello.name, env.hint);
    applet_hello.page(&env, &p);
    check("three lines", p.count, 3);
    check("counted", strcmp(p.line[1], "pressed 2 times"), 0);
    check("turned", strcmp(p.line[2], "turned  -2"), 0);
    for (uint8_t i = 0; i < p.count; i++) {
        check("fits", ui_page_line_fits(&p, i), 1);
    }

    memset(g_fb, 0, sizeof g_fb);
    applet_render(&applet_hello, &env, &g_canvas);
    check("drawn as a page: header", px(UI_SCREEN_W - 2, 1), ui_theme()->header);
    check("with its title", has_colour(3, 3, 40, UI_FONT_H, ui_theme()->on_header), 1);
    check("and the hint, on the right", has_colour(64, UI_SCREEN_H - 12, 62, UI_FONT_H,
                                                   ui_theme()->text_dim), 1);
    check("enter resets", applet_hello.enter(&env) && (ui_page_init(&p, "", ""),
          applet_hello.page(&env, &p), strcmp(p.line[1], "pressed 0 times") == 0), 1);

    /* An invalid applet draws nothing rather than half a screen. */
    applet_t bad = applet_hello;
    bad.page = NULL;
    memset(g_fb, 0xAB, sizeof g_fb);
    applet_render(&bad, &env, &g_canvas);
    check("invalid: nothing drawn", px(64, 64), 0xABAB);
    applet_bind_env(NULL);
}

int main(int argc, char **argv) {
    if (argc < 2 || !load_db(argv[1])) {
        printf("usage: test_applets <bn6g.chdb>\n");
        return 1;
    }
    test_registry();
    test_env();
    test_handlers();
    test_menu_placement();
    test_test_pattern();
    test_chip_dice();
    test_hello();
    if (g_fail) {
        printf("%d FAILURES\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
