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

extern const applet_t applet_hello, applet_test_pattern, applet_chip_dice,
                      applet_battle_chip;
extern uint16_t battle_chip_row_slot(uint16_t row);

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
static void  *g_bn4;        /* BN4, where index and wire ID mostly differ */
static size_t g_bn4_len;
static int    g_opens;
static cg_game_t g_pick = CG_GAME_BN6_US;   /* what host_choose picks */

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
    if (game == CG_GAME_BN4 && g_bn4) {
        return chipdb_open(db, g_bn4, g_bn4_len);
    }
    return game == CG_GAME_BN6_US && chipdb_open(db, g_blob, g_blob_len);
}

/* bn4r.chdb, next to the database the suite was handed. */
static void load_bn4(const char *bn6_path) {
    char path[512];
    snprintf(path, sizeof path, "%s", bn6_path);
    char *slash = strrchr(path, '/');
    snprintf(slash ? slash + 1 : path, sizeof path - (size_t)(slash ? slash + 1 - path : 0),
             "bn4r.chdb");
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_bn4 = malloc((size_t)n);
    g_bn4_len = fread(g_bn4, 1, (size_t)n, f);
    fclose(f);
}

static uint32_t g_seq;
static uint32_t counting_random(void) { return g_seq++ * 2654435761u; }

/* ---- the registry -------------------------------------------------------- */

static void test_registry(void) {
    printf("registry\n");
    /* applets/applets.def: test_pattern, chip_dice; hello commented out. */
    /* applets/applets.def: battle_chip, test_pattern, chip_dice; hello is
     * commented out. */
    check("three listed", applet_count(), 3);
    check("Battle Chip first", applet_at(0) == &applet_battle_chip, 1);
    check("then Test Pattern", applet_at(1) == &applet_test_pattern, 1);
    check("then Chip Dice", applet_at(2) == &applet_chip_dice, 1);
    check("past the end", applet_at(3) == NULL, 1);
    check("Battle Chip is valid", applet_valid(&applet_battle_chip), 1);

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
          applet_for_mode((app_mode_t)(APP_MODE_APPLET_FIRST + 1)) == &applet_test_pattern, 1);
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
    check("one per applet", applet_handlers(h, APPLET_MAX + 2), 3);
    check("modes from APP_MODE_APPLET_FIRST", h[0].mode, APP_MODE_APPLET_FIRST);
    check("in order", h[2].mode, APP_MODE_APPLET_FIRST + 2);
    check("ctx is the applet", h[2].ctx == &applet_chip_dice, 1);
    check("requires carried", h[1].requires, HW_CAP_DISPLAY);
    check("needs_game carried", h[2].needs_game, 1);
    check("Battle Chip needs the link port",
          h[0].requires, HW_CAP_DISPLAY | HW_CAP_LINK_PORT);
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
                t->shell.game_cursor = (uint8_t)g_pick;
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
    int bc = -1;
    for (uint8_t i = 0; i < shell_item_count(&t.shell); i++) {
        if (!strcmp(shell_label_at(&t.shell, i), "Battle Chip")) bc = i;
    }
    check("Battle Chip listed with a link port", bc >= 0, 1);
    check("first of the applets", tp, bc + 1);
    host_init(&t, HW_CAP_DISPLAY);
    bc = -1;
    for (uint8_t i = 0; i < shell_item_count(&t.shell); i++) {
        if (!strcmp(shell_label_at(&t.shell, i), "Battle Chip")) bc = i;
    }
    check("hidden without one", bc, -1);
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
    t.shell.game_cursor = (uint8_t)CG_GAME_BN5;   /* the fake has no BN5 */
    shell_select(&t.shell);
    check("no database: refused", app_dispatch_from_shell(&t.d, &t.shell), APP_DISPATCH_REFUSED);

    /* An image with no databases at all. */
    host_init(&t, HW_CAP_DISPLAY);
    t.env.open_db = NULL;
    check("no open_db: refused", host_choose(&t, "Chip Dice"), 0);
    check("with the reason", t.d.last_status, APP_DISPATCH_REFUSED);
}

/* ---- Battle Chip -------------------------------------------------------- */

/* A gate and a reader, recording what the applet asks of them. */
static struct {
    bool      started, stopped, busy, linked;
    cg_game_t game;
    uint16_t  sent[32];
    int       nsent;
    int       cancels;
    uint16_t  in_reader;
    int       saves;
} g_gate;
static uint8_t g_favs[64];

static bool gate_start(cg_game_t g) { g_gate.started = true; g_gate.game = g; return true; }
static bool gate_start_fails(cg_game_t g) { (void)g; return false; }
static void gate_stop(void) { g_gate.stopped = true; }
static bool gate_send(uint16_t id) {
    if (g_gate.busy) return false;
    if (g_gate.nsent < 32) g_gate.sent[g_gate.nsent++] = id;
    return true;
}
static bool gate_cancel(void) {
    bool was = g_gate.busy;
    g_gate.busy = false;
    g_gate.cancels++;
    return was;
}
static bool gate_linked(void) { return g_gate.linked; }
static uint16_t read_chip(void) { return g_gate.in_reader; }
static void favs_changed(void) { g_gate.saves++; }

static void bc_host(host_t *t, bool reader) {
    memset(&g_gate, 0, sizeof g_gate);
    memset(g_favs, 0, sizeof g_favs);
    host_init(t, HW_CAP_DISPLAY | HW_CAP_LINK_PORT);
    t->env.gate_start  = gate_start;
    t->env.gate_stop   = gate_stop;
    t->env.gate_send   = gate_send;
    t->env.gate_cancel = gate_cancel;
    t->env.gate_linked = gate_linked;
    t->env.read_chip   = reader ? read_chip : NULL;
    t->env.favorites   = g_favs;
    t->env.favorites_len = sizeof g_favs;
    t->env.favorites_changed = favs_changed;
}

static app_mode_t bc_mode(void) { return (app_mode_t)APP_MODE_APPLET_FIRST; }

/* The footer's status line, as drawn: does it hold this colour? */
static bool status_in(uint16_t colour) {
    return has_colour(50, UI_SCREEN_H - UI_FOOTER_H + 2, 76, UI_FOOTER_H - 4, colour);
}

static void test_battle_chip(void) {
    printf("battle chip\n");
    host_t t;
    chipdb_t db;
    check("the BN4 database is there", g_bn4 != NULL, 1);
    chipdb_open(&db, g_bn4, g_bn4_len);
    g_pick = CG_GAME_BN4;

    /* Without a gate service -- an image with no link port -- it refuses. */
    bc_host(&t, false);
    t.env.gate_start = NULL;
    check("no gate: refused", host_choose(&t, "Battle Chip"), 0);
    bc_host(&t, false);
    t.env.gate_start = gate_start_fails;
    check("gate unavailable: refused", host_choose(&t, "Battle Chip"), 0);
    bc_host(&t, false);
    t.env.open_db = NULL;
    check("no database: refused", host_choose(&t, "Battle Chip"), 0);

    bc_host(&t, false);
    check("starts with a game", host_choose(&t, "Battle Chip"), 1);
    check("asked which game", t.shell.has_game, 1);
    check("the gate started", g_gate.started, 1);
    check("as that game's gate", g_gate.game, CG_GAME_BN4);
    host_draw(&t);
    check("the chip list is drawn", px(UI_SCREEN_W - 6, 1), ui_theme()->header);
    check("waiting, in warn", status_in(ui_theme()->warn), 1);

    /*
     * The chip sent is the wire ID of the chip on screen -- not its table
     * index, which the old built-in mode sent and which names a different
     * chip for most of the list. Walk to a row where the two differ, so the
     * old bug could not pass this.
     */
    int found = -1;
    for (int row = 0; row < 200; row++) {
        uint16_t slot = battle_chip_row_slot(row);
        chip_record_t r;
        if (slot != 0xFFFF && chipdb_get(&db, slot, &r) &&
            chipdb_id_for_slot(&db, slot) != CHIPDB_NO_ID &&
            chipdb_id_for_slot(&db, slot) != r.index) {
            found = row;
            break;
        }
    }
    check("a row where index and wire ID differ", found >= 0, 1);
    applet_input_to(bc_mode(), APPLET_IN_MOVE, found);
    check("confirm is used", applet_input_to(bc_mode(), APPLET_IN_CONFIRM, 0), 1);
    check("one chip sent", g_gate.nsent, 1);
    uint16_t slot = battle_chip_row_slot(found);
    check("its wire ID, not its index", g_gate.sent[0], chipdb_id_for_slot(&db, slot));
    host_draw(&t);
    check("sent, in good", status_in(ui_theme()->good), 1);

    /* Busy: refused, said, and Back cancels it rather than leaving. */
    g_gate.busy = true;
    applet_input_to(bc_mode(), APPLET_IN_CONFIRM, 0);
    check("nothing sent while busy", g_gate.nsent, 1);
    host_draw(&t);
    check("busy, in warn", status_in(ui_theme()->warn), 1);
    check("Back cancels the chip going out", applet_input_to(bc_mode(), APPLET_IN_BACK, 0), 1);
    check("it was cancelled", g_gate.busy, 0);
    check("then Back is the host's", applet_input_to(bc_mode(), APPLET_IN_BACK, 0), 0);

    /* Linked, from the gate. */
    g_gate.linked = true;
    t.env.now_ms += 10;
    app_dispatch_tick(&t.d);
    host_draw(&t);
    check("linked, in good", status_in(ui_theme()->good), 1);

    /* Tabs change what is listed. */
    host_draw(&t);
    uint16_t before[UI_SCREEN_W * UI_SCREEN_H];
    memcpy(before, g_fb, sizeof before);
    check("tab is used", applet_input_to(bc_mode(), APPLET_IN_TAB, 1), 1);
    host_draw(&t);
    check("the list changed", memcmp(before, g_fb, sizeof before) != 0, 1);
    memcpy(before, g_fb, sizeof before);
    check("both ways", applet_input_to(bc_mode(), APPLET_IN_TAB, -1), 1);
    host_draw(&t);
    check("the list changed back", memcmp(before, g_fb, sizeof before) != 0, 1);

    /* A favourite is kept by the host, and saved. */
    int set_before = 0;
    for (unsigned i = 0; i < sizeof g_favs; i++) set_before += g_favs[i] != 0;
    check("mark is used", applet_input_to(bc_mode(), APPLET_IN_MARK, 0), 1);
    int set_after = 0;
    for (unsigned i = 0; i < sizeof g_favs; i++) set_after += g_favs[i] != 0;
    check("written to the host's favourites", set_after > set_before, 1);
    check("and a save asked for", g_gate.saves, 1);

    /* Sort cycles four orders, then Move, where confirm picks and places
     * rather than sending. */
    for (int i = 0; i < 4; i++) {
        check("menu is used", applet_input_to(bc_mode(), APPLET_IN_MENU, 0), 1);
    }
    int sent = g_gate.nsent;
    applet_input_to(bc_mode(), APPLET_IN_CONFIRM, 0);     /* pick */
    host_draw(&t);
    check("a held row is drawn", has_colour(0, UI_LIST_Y, 100, UI_LIST_H, ui_theme()->held), 1);
    check("Back drops the held chip", applet_input_to(bc_mode(), APPLET_IN_BACK, 0), 1);
    applet_input_to(bc_mode(), APPLET_IN_CONFIRM, 0);     /* pick again */
    applet_input_to(bc_mode(), APPLET_IN_MOVE, 2);
    applet_input_to(bc_mode(), APPLET_IN_CONFIRM, 0);     /* place */
    check("Move mode sends nothing", g_gate.nsent, sent);
    applet_input_to(bc_mode(), APPLET_IN_MENU, 0);        /* back to library */
    applet_input_to(bc_mode(), APPLET_IN_CONFIRM, 0);
    check("and sending works again after", g_gate.nsent, sent + 1);

    /* Leaving: the gate is cleared and stopped. */
    shell_back(&t.shell);
    app_dispatch_from_shell(&t.d, &t.shell);
    check("left", app_dispatch_running(&t.d), 0);
    check("a chip in flight is dropped", g_gate.cancels >= 2, 1);
    check("the gate stopped", g_gate.stopped, 1);

    /* A chip with no wire ID cannot be sent, and the screen says so. */
    bc_host(&t, false);
    host_choose(&t, "Battle Chip");
    int noid = -1;
    for (int row = 0; row < 400; row++) {
        uint16_t sl = battle_chip_row_slot((uint16_t)row);
        if (sl != 0xFFFF && chipdb_id_for_slot(&db, sl) == CHIPDB_NO_ID) {
            noid = row;
            break;
        }
    }
    if (noid >= 0) {
        applet_input_to(bc_mode(), APPLET_IN_MOVE, noid);
        applet_input_to(bc_mode(), APPLET_IN_CONFIRM, 0);
        check("no wire ID: nothing sent", g_gate.nsent, 0);
        host_draw(&t);
        check("and a warning", status_in(ui_theme()->warn), 1);
    }
    shell_back(&t.shell);
    app_dispatch_from_shell(&t.d, &t.shell);
    g_pick = CG_GAME_BN6_US;
}

/* The reader: a chip in the slot is sent once, as a real gate sends it. */
static void test_battle_chip_reader(void) {
    printf("battle chip: the reader\n");
    host_t t;
    chipdb_t db;
    chipdb_open(&db, g_blob, g_blob_len);
    bc_host(&t, true);
    host_choose(&t, "Battle Chip");

    uint16_t id = chipdb_id_for_slot(&db, 5);
    check("a chip with a wire ID", id != CHIPDB_NO_ID, 1);

    t.env.now_ms = 1000;
    app_dispatch_tick(&t.d);
    check("empty slot: nothing sent", g_gate.nsent, 0);

    g_gate.in_reader = id;
    t.env.now_ms += 100;
    app_dispatch_tick(&t.d);
    check("not read more often than every 250 ms", g_gate.nsent, 0);
    t.env.now_ms += 200;
    app_dispatch_tick(&t.d);
    check("inserted: sent", g_gate.nsent, 1);
    check("that chip", g_gate.sent[0], id);
    host_draw(&t);
    check("said, in good", status_in(ui_theme()->good), 1);

    for (int i = 0; i < 4; i++) {
        t.env.now_ms += 300;
        app_dispatch_tick(&t.d);
    }
    check("left in: sent once", g_gate.nsent, 1);

    g_gate.in_reader = CG_CHIP_NONE;
    t.env.now_ms += 300;
    app_dispatch_tick(&t.d);
    g_gate.in_reader = id;
    t.env.now_ms += 300;
    app_dispatch_tick(&t.d);
    check("out and in again: sent again", g_gate.nsent, 2);

    g_gate.in_reader = 0x7FFF;   /* outside the map */
    t.env.now_ms += 300;
    app_dispatch_tick(&t.d);
    check("an unknown chip is not sent", g_gate.nsent, 2);
    host_draw(&t);
    check("and says so, in warn", status_in(ui_theme()->warn), 1);

    shell_back(&t.shell);
    app_dispatch_from_shell(&t.d, &t.shell);
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
    load_bn4(argv[1]);
    test_registry();
    test_env();
    test_handlers();
    test_menu_placement();
    test_test_pattern();
    test_chip_dice();
    test_battle_chip();
    test_battle_chip_reader();
    test_hello();
    if (g_fail) {
        printf("%d FAILURES\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
