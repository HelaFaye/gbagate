/*
 * app_modes.h -- the layer between "a mode was chosen" and "a mode is running".
 *
 * ui_shell.c decides *which* mode the user picked. The subsystems that serve
 * those modes -- chipgate.c, gba_multiboot.c, chip_reader.c, the ESP-NOW
 * transport -- all exist and are tested. Nothing joined the two: shell_select()
 * set SHELL_SCREEN_RUNNING and no production code ever read it. See
 * PROJECT_STATUS.md section 2, "Designed but not built".
 *
 * Why this is a portable module rather than a few lines in gate_app.c
 * ------------------------------------------------------------------
 * gate_app.c is ESP-IDF-only -- esp_log.h, FreeRTOS -- so it cannot be built
 * by the host suites. Putting the dispatch decisions there would make them
 * untestable, which is the exact property that let the original gap survive:
 * every module either side of the seam was well covered, and the seam was not
 * covered at all. So the decisions live here, injected handlers do the work,
 * and gate_app.c keeps only the glue.
 *
 * What this refuses to do, and why each guard is here
 * --------------------------------------------------
 * The shell already hides modes whose hardware is absent, so re-checking caps
 * looks redundant. It is not. The shell's filter runs once at shell_init();
 * anything that enters a mode by another route -- a saved "last mode", a
 * diagnostics jump, a future resume-after-reset -- bypasses it entirely.
 *
 * The game guard is the one that catches a real latent bug. ui_shell_t keeps
 * chosen_game and has_game as separate fields, and has_game is only cleared on
 * the path through shell_select(). A mode that needs a game can therefore be
 * reached holding a stale cg_game_t from a previous selection, which would
 * start a gate announcing the wrong identity -- a fault that presents as "the
 * game ignores the gate", with nothing on screen to suggest why.
 */

#ifndef APP_MODES_H
#define APP_MODES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chipgate.h"
#include "ui_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_DISPATCH_OK = 0,
    APP_DISPATCH_NO_HANDLER,    /* no handler registered for that mode      */
    APP_DISPATCH_MISSING_CAPS,  /* the hardware the mode needs is absent    */
    APP_DISPATCH_NEEDS_GAME,    /* mode is per-game, none was supplied      */
    APP_DISPATCH_BUSY,          /* something is already running             */
    APP_DISPATCH_REFUSED        /* the handler's enter() said no            */
} app_dispatch_status_t;

/*
 * One mode's behaviour, injected.
 *
 * enter(): take the mode up. Return false to refuse -- a missing database, a
 *          USB volume the host currently owns, no ROM staged. Refusing is
 *          normal, not exceptional, so it costs a status rather than a trap.
 * exit():  tear down. Must tolerate being called after a refused enter().
 * tick():  service one iteration while running. May be NULL.
 */
typedef struct {
    app_mode_t mode;
    bool (*enter)(void *ctx, cg_game_t game, bool has_game);
    void (*exit)(void *ctx);
    void (*tick)(void *ctx);
} app_mode_handler_t;

typedef struct {
    const app_mode_handler_t *handlers;
    size_t                    handler_count;
    void                     *ctx;
    uint32_t                  caps;

    /* live state */
    bool       running;
    app_mode_t active;
    cg_game_t  game;
    bool       has_game;

    /* observability -- a diagnostics screen can show why a mode refused */
    uint32_t              entries;
    uint32_t              rejections;
    app_dispatch_status_t last_status;
} app_dispatch_t;

/*
 * Bind a handler table. caps is the same mask shell_init() was given; passing
 * a different one is a bug, and the mismatch would show as a mode that lists
 * but will not start.
 */
void app_dispatch_init(app_dispatch_t *d, const app_mode_handler_t *handlers,
                       size_t count, uint32_t caps, void *ctx);

/* Take a mode up. Returns APP_DISPATCH_OK only if it is now running. */
app_dispatch_status_t app_dispatch_enter(app_dispatch_t *d, app_mode_t mode,
                                         cg_game_t game, bool has_game);

/*
 * The seam that was missing: drive dispatch straight from the shell.
 *
 * Does nothing unless the shell has reached SHELL_SCREEN_RUNNING, so it is
 * safe to call every UI iteration. Idempotent while the same mode runs --
 * calling it in a loop re-enters nothing.
 */
app_dispatch_status_t app_dispatch_from_shell(app_dispatch_t *d,
                                              const ui_shell_t *s);

/* Bring the running mode down. Safe when nothing is running. */
void app_dispatch_exit(app_dispatch_t *d);

/* Service the running mode. Safe when nothing is running, or tick is NULL. */
void app_dispatch_tick(app_dispatch_t *d);

static inline bool app_dispatch_running(const app_dispatch_t *d) {
    return d->running;
}

const char *app_dispatch_status_name(app_dispatch_status_t s);

#ifdef __cplusplus
}
#endif

#endif /* APP_MODES_H */
