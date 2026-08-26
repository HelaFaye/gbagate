/*
 * app_modes.c -- see app_modes.h
 */

#include <string.h>

#include "app_modes.h"
#include "hw_caps.h"

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

    if (mode >= APP_MODE_COUNT) {
        return finish(d, APP_DISPATCH_NO_HANDLER);
    }

    const app_mode_handler_t *h = find_handler(d, mode);
    if (!h || !h->enter) {
        return finish(d, APP_DISPATCH_NO_HANDLER);
    }

    if ((mode_requires(mode) & ~d->caps) != 0) {
        return finish(d, APP_DISPATCH_MISSING_CAPS);
    }

    /* The stale-game guard. has_game is authoritative; the cg_game_t beside it
     * means nothing without it. */
    if (shell_mode_needs_game(mode)) {
        if (!has_game || game >= CG_GAME_COUNT) {
            return finish(d, APP_DISPATCH_NEEDS_GAME);
        }
    }

    if (!h->enter(d->ctx, game, has_game)) {
        /* enter() may have taken resources before deciding to refuse. */
        if (h->exit) {
            h->exit(d->ctx);
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
        h->exit(d->ctx);
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
        h->tick(d->ctx);
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
