/*
 * test_modes.c -- mode dispatch: the seam between shell_select() and the
 * subsystems that serve a mode.
 *
 *   cc -std=c11 -Wall -Wextra -o test_modes test_modes.c app_modes.c \
 *      ui_shell.c hw_caps.c chipgate.c
 *
 * The gap this covers was invisible for a reason worth repeating: every module
 * on either side of it was well tested, and test_chipview.c even asserts that
 * shell_select() reaches SHELL_SCREEN_RUNNING -- a green check on a state no
 * production code read. So the assertions here are deliberately about the
 * *join*: that choosing actually starts something, that leaving actually stops
 * it, and that the guards refuse the requests that would otherwise start a
 * mode in a quietly wrong configuration.
 */

#include <stdio.h>
#include <string.h>

#include "app_modes.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-44s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static void check_str(const char *what, const char *got, const char *want) {
    if (strcmp(got ? got : "(null)", want) != 0) {
        printf("  FAIL %-44s got \"%s\", want \"%s\"\n",
               what, got ? got : "(null)", want);
        g_fail++;
    }
}

/* ---- a recording fake mode --------------------------------------------- */

typedef struct {
    int       enters, exits, ticks;
    bool      refuse;      /* make enter() say no        */
    cg_game_t saw_game;
    bool      saw_has_game;
} fake_mode_t;

typedef struct {
    fake_mode_t gate, settings, diag;
} fakes_t;

static bool gate_enter(void *ctx, cg_game_t game, bool has_game) {
    fakes_t *f = ctx;
    f->gate.enters++;
    f->gate.saw_game     = game;
    f->gate.saw_has_game = has_game;
    return !f->gate.refuse;
}
static void gate_exit(void *ctx) { ((fakes_t *)ctx)->gate.exits++; }
static void gate_tick(void *ctx) { ((fakes_t *)ctx)->gate.ticks++; }

static bool settings_enter(void *ctx, cg_game_t game, bool has_game) {
    fakes_t *f = ctx;
    (void)game;
    f->settings.enters++;
    f->settings.saw_has_game = has_game;
    return !f->settings.refuse;
}
static void settings_exit(void *ctx) { ((fakes_t *)ctx)->settings.exits++; }

static bool diag_enter(void *ctx, cg_game_t game, bool has_game) {
    (void)game; (void)has_game;
    ((fakes_t *)ctx)->diag.enters++;
    return true;
}
static void diag_exit(void *ctx) { ((fakes_t *)ctx)->diag.exits++; }

static const app_mode_handler_t k_handlers[] = {
    { .mode = APP_MODE_CHIP_GATE, .enter = gate_enter, .exit = gate_exit, .tick = gate_tick },
    { .mode = APP_MODE_SETTINGS, .enter = settings_enter, .exit = settings_exit, .tick = NULL },
    { .mode = APP_MODE_DIAGNOSTICS, .enter = diag_enter, .exit = diag_exit, .tick = NULL },
};
#define HANDLER_COUNT (sizeof k_handlers / sizeof k_handlers[0])

#define FULL_CAPS (HW_CAP_DISPLAY | HW_CAP_LINK_PORT | HW_CAP_USB | \
                   HW_CAP_WIFI | HW_CAP_CHIP_READER)

static void setup(app_dispatch_t *d, fakes_t *f, uint32_t caps) {
    memset(f, 0, sizeof *f);
    app_dispatch_init(d, k_handlers, HANDLER_COUNT, caps, f);
}

/* ---- the basic join ----------------------------------------------------- */

static void test_enter_and_exit(void) {
    printf("enter and exit\n");

    app_dispatch_t d;
    fakes_t f;
    setup(&d, &f, FULL_CAPS);

    check("nothing running at init", app_dispatch_running(&d), 0);

    check("gate enters",
          app_dispatch_enter(&d, APP_MODE_CHIP_GATE, CG_GAME_BN5, true),
          APP_DISPATCH_OK);
    check("running", app_dispatch_running(&d), 1);
    check("enter called once", f.gate.enters, 1);
    check("game passed through", f.gate.saw_game, CG_GAME_BN5);
    check("has_game passed through", f.gate.saw_has_game, 1);

    app_dispatch_tick(&d);
    app_dispatch_tick(&d);
    check("tick routed to the running mode", f.gate.ticks, 2);

    app_dispatch_exit(&d);
    check("exit called", f.gate.exits, 1);
    check("no longer running", app_dispatch_running(&d), 0);

    /* Exit must be idempotent: a UI that calls it on every back-press should
     * not need to know whether anything was up. */
    app_dispatch_exit(&d);
    check("second exit is a no-op", f.gate.exits, 1);

    app_dispatch_tick(&d);
    check("tick while idle is a no-op", f.gate.ticks, 2);
}

/* ---- the guards --------------------------------------------------------- */

static void test_no_handler(void) {
    printf("unregistered mode\n");

    app_dispatch_t d;
    fakes_t f;
    setup(&d, &f, FULL_CAPS);

    /* Multiboot is a real mode with no handler bound in this table. */
    check("refuses an unbound mode",
          app_dispatch_enter(&d, APP_MODE_MULTIBOOT, CG_GAME_BN4, false),
          APP_DISPATCH_NO_HANDLER);
    check("still idle", app_dispatch_running(&d), 0);

    check("refuses a mode out of range",
          app_dispatch_enter(&d, (app_mode_t)99, CG_GAME_BN4, false),
          APP_DISPATCH_NO_HANDLER);
}

static void test_missing_caps(void) {
    printf("hardware absent\n");

    /* A board with a display but no link port. The shell would not list the
     * gate at all; this checks the second line of defence, for anything that
     * reaches dispatch without passing the menu filter. */
    app_dispatch_t d;
    fakes_t f;
    setup(&d, &f, HW_CAP_DISPLAY);

    check("gate refused without a link port",
          app_dispatch_enter(&d, APP_MODE_CHIP_GATE, CG_GAME_BN4, true),
          APP_DISPATCH_MISSING_CAPS);
    check("enter never called", f.gate.enters, 0);
    check("still idle", app_dispatch_running(&d), 0);

    /* Diagnostics requires nothing, so it must remain reachable on a board
     * with nothing fitted -- the screen that explains a missing part cannot
     * itself be gated on parts. */
    check("diagnostics still reachable",
          app_dispatch_enter(&d, APP_MODE_DIAGNOSTICS, CG_GAME_BN4, false),
          APP_DISPATCH_OK);
}

static void test_needs_game(void) {
    printf("stale game guard\n");

    app_dispatch_t d;
    fakes_t f;
    setup(&d, &f, FULL_CAPS);

    /*
     * The bug this exists to stop: ui_shell_t carries chosen_game and has_game
     * separately, so a per-game mode can be reached holding a leftover
     * cg_game_t with has_game false. Starting on it would announce the wrong
     * gate identity, and the console's only symptom is silence.
     */
    check("per-game mode refused without a game",
          app_dispatch_enter(&d, APP_MODE_CHIP_GATE, CG_GAME_BN6_US, false),
          APP_DISPATCH_NEEDS_GAME);
    check("enter never called", f.gate.enters, 0);

    check("and refused with an out-of-range game",
          app_dispatch_enter(&d, APP_MODE_CHIP_GATE, (cg_game_t)42, true),
          APP_DISPATCH_NEEDS_GAME);
    check("still never called", f.gate.enters, 0);

    /* A mode that needs no game is unaffected by has_game either way. */
    check("gameless mode starts without one",
          app_dispatch_enter(&d, APP_MODE_SETTINGS, CG_GAME_BN4, false),
          APP_DISPATCH_OK);
    check("told it had no game", f.settings.saw_has_game, 0);
}

static void test_busy(void) {
    printf("already running\n");

    app_dispatch_t d;
    fakes_t f;
    setup(&d, &f, FULL_CAPS);

    check("first starts",
          app_dispatch_enter(&d, APP_MODE_SETTINGS, CG_GAME_BN4, false),
          APP_DISPATCH_OK);
    check("second refused",
          app_dispatch_enter(&d, APP_MODE_DIAGNOSTICS, CG_GAME_BN4, false),
          APP_DISPATCH_BUSY);
    check("the second mode never entered", f.diag.enters, 0);
    check("the first is untouched", f.settings.exits, 0);
}

static void test_refused(void) {
    printf("handler refuses\n");

    app_dispatch_t d;
    fakes_t f;
    setup(&d, &f, FULL_CAPS);
    f.gate.refuse = true;

    check("refusal reported",
          app_dispatch_enter(&d, APP_MODE_CHIP_GATE, CG_GAME_BN4, true),
          APP_DISPATCH_REFUSED);
    check("not running", app_dispatch_running(&d), 0);

    /* enter() may have taken a resource before deciding it could not continue,
     * so exit() is called to unwind it. Without this a refused mode leaks
     * whatever it had acquired, and the next attempt fails for a new reason. */
    check("unwound after refusing", f.gate.exits, 1);

    /* And the dispatcher is left usable. */
    f.gate.refuse = false;
    check("recovers",
          app_dispatch_enter(&d, APP_MODE_CHIP_GATE, CG_GAME_BN4, true),
          APP_DISPATCH_OK);
}

/* ---- driven from the shell, which is the real path --------------------- */

static void test_from_shell(void) {
    printf("driven from the shell\n");

    app_dispatch_t d;
    fakes_t f;
    setup(&d, &f, FULL_CAPS);

    ui_shell_t s;
    shell_init(&s, FULL_CAPS);

    /* Sitting on the mode list: nothing should start. */
    app_dispatch_from_shell(&d, &s);
    check("idle while choosing", app_dispatch_running(&d), 0);
    check("nothing entered", f.gate.enters, 0);

    /* Battle Chip is first and needs a game, so this only advances a screen. */
    check("gate does not start yet", shell_select(&s), 0);
    app_dispatch_from_shell(&d, &s);
    check("still idle on the game list", app_dispatch_running(&d), 0);

    /* Choose a game -- now it should actually run, which is the whole point. */
    check("game chosen", shell_select(&s), 1);
    check("dispatch takes it up",
          app_dispatch_from_shell(&d, &s), APP_DISPATCH_OK);
    check("running", app_dispatch_running(&d), 1);
    check("entered once", f.gate.enters, 1);
    check("with the chosen game", f.gate.saw_game, s.chosen_game);

    /* Calling it every UI iteration must cost nothing. */
    for (int i = 0; i < 100; i++) {
        check_str("stays ok",
                  app_dispatch_status_name(app_dispatch_from_shell(&d, &s)),
                  "ok");
    }
    check("still exactly one entry", f.gate.enters, 1);
    check("and no spurious exits", f.gate.exits, 0);

    /* Backing out of a running mode must bring it down. The shell is the
     * source of truth; the caller does not have to pair enter with exit. */
    check("back leaves running", shell_back(&s), 1);
    app_dispatch_from_shell(&d, &s);
    check("mode stopped", app_dispatch_running(&d), 0);
    check("exit called exactly once", f.gate.exits, 1);
}

static void test_from_shell_swap(void) {
    printf("switching modes\n");

    app_dispatch_t d;
    fakes_t f;
    setup(&d, &f, FULL_CAPS);

    ui_shell_t s;
    shell_init(&s, FULL_CAPS);

    /* Start Settings. */
    while (strcmp(shell_current_label(&s), "Settings") != 0) {
        shell_move(&s, 1);
    }
    check("settings starts at once", shell_select(&s), 1);
    check("dispatched", app_dispatch_from_shell(&d, &s), APP_DISPATCH_OK);
    check("settings up", f.settings.enters, 1);

    /* Go back and pick Diagnostics instead. */
    check("back", shell_back(&s), 1);
    app_dispatch_from_shell(&d, &s);
    check("settings came down", f.settings.exits, 1);

    while (strcmp(shell_current_label(&s), "Diagnostics") != 0) {
        shell_move(&s, 1);
    }
    check("diagnostics starts", shell_select(&s), 1);
    check("dispatched", app_dispatch_from_shell(&d, &s), APP_DISPATCH_OK);
    check("diagnostics up", f.diag.enters, 1);
    check("settings not re-entered", f.settings.enters, 1);
}

static void test_from_shell_unbound(void) {
    printf("shell offers a mode with no handler\n");

    /*
     * The shell lists every mode whose hardware is present, but this table
     * binds only three. Selecting one of the others must fail loudly and leave
     * the dispatcher idle rather than half-up -- during bring-up this is the
     * normal state of the world, and it should read as "not wired yet" instead
     * of hanging on a blank screen.
     */
    app_dispatch_t d;
    fakes_t f;
    setup(&d, &f, FULL_CAPS);

    ui_shell_t s;
    shell_init(&s, FULL_CAPS);
    while (strcmp(shell_current_label(&s), "Send Program") != 0) {
        shell_move(&s, 1);
    }
    check("multiboot starts at once", shell_select(&s), 1);
    check("but has no handler",
          app_dispatch_from_shell(&d, &s), APP_DISPATCH_NO_HANDLER);
    check("dispatcher left idle", app_dispatch_running(&d), 0);
    check("counted as a rejection", d.rejections, 1);
    check_str("and says why",
              app_dispatch_status_name(d.last_status), "no handler");
}

/* ---- handlers the mode table does not describe: applets ----------------- */

typedef struct { int enters, exits, ticks; } own_t;

static bool own_enter(void *ctx, cg_game_t game, bool has_game) {
    (void)game; (void)has_game;
    ((own_t *)ctx)->enters++;
    return true;
}
static void own_exit(void *ctx) { ((own_t *)ctx)->exits++; }
static void own_tick(void *ctx) { ((own_t *)ctx)->ticks++; }

/*
 * A handler's own ctx reaches its callbacks instead of the dispatcher's; its
 * requires and needs_game guard it like a built-in mode's table entry; and
 * applet mode numbers are accepted while others past the table are not.
 */
static void test_applet_handlers(void) {
    printf("applet handlers\n");
    own_t a = {0}, b = {0};
    const app_mode_t ma = (app_mode_t)APP_MODE_APPLET_FIRST;
    const app_mode_t mb = (app_mode_t)(APP_MODE_APPLET_FIRST + 1);
    const app_mode_handler_t hs[] = {
        { .mode = ma, .enter = own_enter, .exit = own_exit, .tick = own_tick,
          .ctx = &a, .requires = HW_CAP_DISPLAY },
        { .mode = mb, .enter = own_enter, .exit = own_exit, .tick = own_tick,
          .ctx = &b, .needs_game = true },
    };
    app_dispatch_t d;
    fakes_t f;
    memset(&f, 0, sizeof f);
    app_dispatch_init(&d, hs, 2, HW_CAP_DISPLAY, &f);

    check("applet enters", app_dispatch_enter(&d, ma, CG_GAME_BN6_US, false), APP_DISPATCH_OK);
    app_dispatch_tick(&d);
    app_dispatch_exit(&d);
    check("its ctx got enter", a.enters, 1);
    check("and tick", a.ticks, 1);
    check("and exit", a.exits, 1);
    check("the other did not", b.enters + b.ticks + b.exits, 0);

    check("needs a game: refused without one",
          app_dispatch_enter(&d, mb, CG_GAME_BN6_US, false), APP_DISPATCH_NEEDS_GAME);
    check("runs with one", app_dispatch_enter(&d, mb, CG_GAME_BN5, true), APP_DISPATCH_OK);
    app_dispatch_exit(&d);

    app_dispatch_init(&d, hs, 2, 0, &f);
    check("its requires are checked",
          app_dispatch_enter(&d, ma, CG_GAME_BN6_US, false), APP_DISPATCH_MISSING_CAPS);

    check("an unregistered applet number",
          app_dispatch_enter(&d, (app_mode_t)(APP_MODE_APPLET_FIRST + 5), CG_GAME_BN6_US, false),
          APP_DISPATCH_NO_HANDLER);
    check("a number past both ranges",
          app_dispatch_enter(&d, (app_mode_t)(APP_MODE_APPLET_FIRST + SHELL_EXTRA_MAX), CG_GAME_BN6_US, false),
          APP_DISPATCH_NO_HANDLER);
    check("between the ranges",
          app_dispatch_enter(&d, (app_mode_t)APP_MODE_COUNT, CG_GAME_BN6_US, false),
          APP_DISPATCH_NO_HANDLER);
}

/*
 * Every mode the menu can reach has a screen. Walked from the shell itself --
 * every row, and every interface behind Link -- so a mode added to the menu
 * without a screen fails here, the way Infrared reached a placeholder.
 */
static void test_every_menu_entry_has_a_screen(void) {
    printf("every menu entry has a screen\n");
    ui_shell_t s;
    shell_init(&s, 0xFFFFFFFFu);
    shell_add_entry(&s, (app_mode_t)APP_MODE_APPLET_FIRST, "Applet", 0, NULL, false);
    int reached = 0;
    for (uint8_t i = 0; i < s.mode_count; i++) {
        ui_shell_t t = s;
        t.mode_cursor = i;
        bool running = shell_select(&t);
        if (t.screen == SHELL_SCREEN_GAMES) {
            running = shell_select(&t);
        }
        if (t.screen == SHELL_SCREEN_LINK) {
            for (uint8_t k = 0; k < t.link_count; k++) {
                ui_shell_t u = t;
                u.link_cursor = k;
                if (shell_select(&u)) {
                    if (app_mode_route(u.chosen_mode) == APP_ROUTE_NONE) {
                        printf("  FAIL %s has no screen\n", shell_label_at(&u, k));
                        g_fail++;
                    }
                    reached++;
                }
            }
            continue;
        }
        check(s.modes[i]->label, running, 1);
        if (app_mode_route(t.chosen_mode) == APP_ROUTE_NONE) {
            printf("  FAIL %s has no screen\n", s.modes[i]->label);
            g_fail++;
        }
        reached++;
    }
    /* All ten built-in modes and the applet. */
    check("modes reached", reached, APP_MODE_COUNT + 1);
    check("chip gate: the chip list", app_mode_route(APP_MODE_CHIP_GATE), APP_ROUTE_CHIPS);
    check("send program: the browser", app_mode_route(APP_MODE_MULTIBOOT), APP_ROUTE_ROMS);
    check("settings: the editor", app_mode_route(APP_MODE_SETTINGS), APP_ROUTE_SETTINGS);
    check("infrared: a page", app_mode_route(APP_MODE_INFRARED), APP_ROUTE_PAGE);
    check("self test: a page", app_mode_route(APP_MODE_SELFTEST), APP_ROUTE_PAGE);
    check("an applet draws itself",
          app_mode_route((app_mode_t)APP_MODE_APPLET_FIRST), APP_ROUTE_APPLET);
    check("not a mode", app_mode_route(APP_MODE_COUNT), APP_ROUTE_NONE);
}

int main(void) {
    test_enter_and_exit();
    test_no_handler();
    test_missing_caps();
    test_needs_game();
    test_busy();
    test_refused();
    test_from_shell();
    test_from_shell_swap();
    test_from_shell_unbound();
    test_applet_handlers();
    test_every_menu_entry_has_a_screen();

    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
