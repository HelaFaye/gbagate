/*
 * app_modes.c -- see app_modes.h
 */

#include <string.h>

#include "app_modes.h"
#include <stdio.h>
#include "hw_caps.h"
#include "chipgate.h"

/* The capability requirements live in the shell's mode table. Reading them
 * back rather than restating them here means the menu and the dispatcher can
 * never disagree about what a mode needs -- a duplicated table would drift,
 * and the symptom would be a mode that lists but refuses to start. */
static uint32_t mode_requires(app_mode_t mode) {
    size_t n = 0;
    const menu_entry_t *all = shell_all_modes(&n);
    for (size_t i = 0; i < n; i++) {
        if (all[i].id == (int)mode) {
            return all[i].requires;
        }
    }
    return 0;
}

static const app_mode_handler_t *find_handler(const app_dispatch_t *d,
                                              app_mode_t mode) {
    for (size_t i = 0; i < d->handler_count; i++) {
        if (d->handlers[i].mode == mode) {
            return &d->handlers[i];
        }
    }
    return NULL;
}

static void *handler_ctx(const app_dispatch_t *d, const app_mode_handler_t *h) {
    return h->ctx ? h->ctx : d->ctx;
}

static app_dispatch_status_t finish(app_dispatch_t *d,
                                    app_dispatch_status_t s) {
    d->last_status = s;
    if (s != APP_DISPATCH_OK) {
        d->rejections++;
    }
    return s;
}

void app_dispatch_init(app_dispatch_t *d, const app_mode_handler_t *handlers,
                       size_t count, uint32_t caps, void *ctx) {
    memset(d, 0, sizeof *d);
    d->handlers      = handlers;
    d->handler_count = handlers ? count : 0;
    d->caps          = caps;
    d->ctx           = ctx;
}

app_dispatch_status_t app_dispatch_enter(app_dispatch_t *d, app_mode_t mode,
                                         cg_game_t game, bool has_game) {
    /* Checked before the request is examined at all: if something is already
     * up, this request was never considered, and saying so is more useful
     * than reporting whatever else might also be wrong with it. */
    if (d->running) {
        return finish(d, APP_DISPATCH_BUSY);
    }

    if (mode >= APP_MODE_COUNT && !APP_MODE_IS_APPLET(mode)) {
        return finish(d, APP_DISPATCH_NO_HANDLER);
    }

    const app_mode_handler_t *h = find_handler(d, mode);
    if (!h || !h->enter) {
        return finish(d, APP_DISPATCH_NO_HANDLER);
    }

    if (((mode_requires(mode) | h->requires) & ~d->caps) != 0) {
        return finish(d, APP_DISPATCH_MISSING_CAPS);
    }

    /* The stale-game guard. has_game is authoritative; the cg_game_t beside it
     * means nothing without it. */
    if (shell_mode_needs_game(mode) || h->needs_game) {
        if (!has_game || game >= CG_GAME_COUNT) {
            return finish(d, APP_DISPATCH_NEEDS_GAME);
        }
    }

    if (!h->enter(handler_ctx(d, h), game, has_game)) {
        /* enter() may have taken resources before deciding to refuse. */
        if (h->exit) {
            h->exit(handler_ctx(d, h));
        }
        return finish(d, APP_DISPATCH_REFUSED);
    }

    d->running  = true;
    d->active   = mode;
    d->game     = game;
    d->has_game = has_game;
    d->entries++;
    return finish(d, APP_DISPATCH_OK);
}

app_dispatch_status_t app_dispatch_from_shell(app_dispatch_t *d,
                                              const ui_shell_t *s) {
    /*
     * Declarative on purpose: the shell is the single source of truth and this
     * makes dispatch follow it, rather than requiring the caller to pair an
     * enter with an exit by hand. Leaving a mode by any route -- shell_back(),
     * a reset, anything that moves the screen away from RUNNING -- brings the
     * running mode down on the next iteration. An enter/exit pairing left to
     * the caller is precisely the kind of obligation that gets missed on one
     * of several paths.
     */
    if (s->screen != SHELL_SCREEN_RUNNING) {
        if (d->running) {
            app_dispatch_exit(d);
        }
        return finish(d, APP_DISPATCH_OK);
    }

    /* Already up and unchanged: calling this every UI iteration must be free. */
    if (d->running && d->active == s->chosen_mode &&
        d->has_game == s->has_game &&
        (!s->has_game || d->game == s->chosen_game)) {
        return finish(d, APP_DISPATCH_OK);
    }

    /* Same screen, different selection -- swap rather than report BUSY. */
    if (d->running) {
        app_dispatch_exit(d);
    }

    return app_dispatch_enter(d, s->chosen_mode, s->chosen_game, s->has_game);
}

void app_dispatch_exit(app_dispatch_t *d) {
    if (!d->running) {
        return;
    }
    const app_mode_handler_t *h = find_handler(d, d->active);
    if (h && h->exit) {
        h->exit(handler_ctx(d, h));
    }
    d->running  = false;
    d->has_game = false;
}

void app_dispatch_tick(app_dispatch_t *d) {
    if (!d->running) {
        return;
    }
    const app_mode_handler_t *h = find_handler(d, d->active);
    if (h && h->tick) {
        h->tick(handler_ctx(d, h));
    }
}

app_route_t app_mode_route(app_mode_t mode) {
    if (APP_MODE_IS_APPLET(mode)) {
        return APP_ROUTE_APPLET;
    }
    switch (mode) {
    case APP_MODE_MULTIBOOT:     return APP_ROUTE_ROMS;
    case APP_MODE_SETTINGS:      return APP_ROUTE_SETTINGS;
    case APP_MODE_LINK_USB:
    case APP_MODE_LINK_WIRELESS:
    case APP_MODE_USB_STORAGE:
    case APP_MODE_INFRARED:
    case APP_MODE_SELFTEST:
    case APP_MODE_DIAGNOSTICS:   return APP_ROUTE_PAGE;
    default:                     return APP_ROUTE_NONE;
    }
}

const char *app_dispatch_status_name(app_dispatch_status_t s) {
    switch (s) {
    case APP_DISPATCH_OK:           return "ok";
    case APP_DISPATCH_NO_HANDLER:   return "no handler";
    case APP_DISPATCH_MISSING_CAPS: return "hardware absent";
    case APP_DISPATCH_NEEDS_GAME:   return "no game chosen";
    case APP_DISPATCH_BUSY:         return "already running";
    case APP_DISPATCH_REFUSED:      return "refused";
    default:                        return "?";
    }
}

/* ---- mode handlers ----------------------------------------------------- */

static bool link_usb_enter(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx;
    (void)game; (void)has_game;
    return true;
}

static void link_usb_exit(void *ctx) { (void)ctx; }

static void link_usb_tick(void *ctx) { (void)ctx; }

static bool link_wireless_enter(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx;
    (void)game; (void)has_game;
    return true;
}

static void link_wireless_exit(void *ctx) { (void)ctx; }

static void link_wireless_tick(void *ctx) { (void)ctx; }

static bool multiboot_enter(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx;
    (void)game; (void)has_game;
    return true;
}

static void multiboot_exit(void *ctx) { (void)ctx; }

static void multiboot_tick(void *ctx) { (void)ctx; }

static bool usb_storage_enter(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx;
    (void)game; (void)has_game;
    return true;
}

static void usb_storage_exit(void *ctx) { (void)ctx; }

static void usb_storage_tick(void *ctx) { (void)ctx; }

/*
 * Infrared. The transmitter may not be fitted, so enter() reports whether it
 * is rather than refusing: the screen then says "no IR fitted", which is more
 * use than the mode being absent from a menu the user is looking at.
 */
static bool ir_enter(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx; (void)game; (void)has_game;
    return true;
}

static void ir_exit(void *ctx) { (void)ctx; }
static void ir_tick(void *ctx) { (void)ctx; }

static bool selftest_enter(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx;
    (void)game; (void)has_game;
    return true;
}

static void selftest_exit(void *ctx) { (void)ctx; }

static void selftest_tick(void *ctx) { (void)ctx; }

static bool settings_enter(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx;
    (void)game; (void)has_game;
    return true;
}

static void settings_exit(void *ctx) { (void)ctx; }

static void settings_tick(void *ctx) { (void)ctx; }

static bool diagnostics_enter(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx;
    (void)game; (void)has_game;
    return true;
}

static void diagnostics_exit(void *ctx) { (void)ctx; }

static void diagnostics_tick(void *ctx) { (void)ctx; }

/* ---- handler table ---------------------------------------------------- */

static const app_mode_handler_t k_handlers[] = {
    { .mode = APP_MODE_LINK_USB, .enter = link_usb_enter, .exit = link_usb_exit, .tick = link_usb_tick },
    { .mode = APP_MODE_LINK_WIRELESS, .enter = link_wireless_enter, .exit = link_wireless_exit, .tick = link_wireless_tick },
    { .mode = APP_MODE_MULTIBOOT, .enter = multiboot_enter, .exit = multiboot_exit, .tick = multiboot_tick },
    { .mode = APP_MODE_USB_STORAGE, .enter = usb_storage_enter, .exit = usb_storage_exit, .tick = usb_storage_tick },
    { .mode = APP_MODE_INFRARED, .enter = ir_enter, .exit = ir_exit, .tick = ir_tick },
    { .mode = APP_MODE_SELFTEST, .enter = selftest_enter, .exit = selftest_exit, .tick = selftest_tick },
    { .mode = APP_MODE_SETTINGS, .enter = settings_enter, .exit = settings_exit, .tick = settings_tick },
    { .mode = APP_MODE_DIAGNOSTICS, .enter = diagnostics_enter, .exit = diagnostics_exit, .tick = diagnostics_tick },
};

#define HANDLER_COUNT (sizeof k_handlers / sizeof k_handlers[0])

/* ---- exported functions ------------------------------------------------ */


/* ---- the handler table ------------------------------------------------- */

/*
 * A shell-integration block used to live here. It has been removed rather than
 * repaired, because everything it did was already done correctly in
 * gate_app.c and its own version was wrong four ways:
 *
 *   - it defined gate_shell_start(void), colliding with gate_app.c's
 *     gate_shell_start(uint32_t caps). Same symbol, different signature: the
 *     link failed with "multiple definition" once src/ started being globbed,
 *     which is how it surfaced at all;
 *   - it hardcoded the capability mask to every capability, instead of the one
 *     app_main actually probed. Read Chip would have been offered on a board
 *     with no reader, and Link: USB with no USB -- the exact thing hw_caps
 *     exists to prevent;
 *   - each of its three functions declared its own `static` shell and dispatch
 *     objects, so state written by one was invisible to the others;
 *   - and it looped `while (label != "Modes") shell_move(...)` over a menu with
 *     no such entry. shell_move wraps, so that never terminates.
 *
 * The handler table below is the useful part and is kept. gate_app.c owns the
 * shell, with the real probed capabilities.
 */
const app_mode_handler_t *app_modes_table(size_t *count) {
    if (count) {
        *count = HANDLER_COUNT;
    }
    return k_handlers;
}
