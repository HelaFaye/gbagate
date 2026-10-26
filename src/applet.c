/*
 * applet.c -- see applet.h
 *
 * The list is applets/applets.def, included twice: once to declare each
 * applet, once to collect them. Adding an applet is one line there; nothing
 * here changes.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "applet.h"

#define APPLET(name) extern const applet_t applet_##name;
#include "../applets/applets.def"
#undef APPLET

#define APPLET(name) &applet_##name,
static const applet_t *const k_listed[] = {
#include "../applets/applets.def"
    NULL
};
#undef APPLET

#define LISTED (sizeof k_listed / sizeof k_listed[0] - 1)

bool applet_valid(const applet_t *a) {
    return a && a->name && a->name[0] && ((a->page != NULL) != (a->draw != NULL));
}

/* The usable ones, in order, at most APPLET_MAX. Built on first use. */
static const applet_t *s_valid[APPLET_MAX];
static uint8_t         s_count;
static bool            s_built;

static void build(void) {
    if (s_built) {
        return;
    }
    s_built = true;
    for (size_t i = 0; i < LISTED && s_count < APPLET_MAX; i++) {
        if (applet_valid(k_listed[i])) {
            s_valid[s_count++] = k_listed[i];
        }
    }
}

uint8_t applet_count(void) {
    build();
    return s_count;
}

const applet_t *applet_at(uint8_t index) {
    build();
    return index < s_count ? s_valid[index] : NULL;
}

void applet_render(const applet_t *a, const applet_env_t *env,
                   const ui_canvas_t *canvas) {
    if (!applet_valid(a)) {
        return;
    }
    if (a->draw) {
        a->draw(env, canvas);
        return;
    }
    /* The host's hint is the Back half of a footer ("hold back"), so it sits
     * on the right like every other page's; the left is the applet's own. */
    ui_page_t page;
    ui_page_init(&page, a->name, NULL);
    snprintf(page.footer_right, sizeof page.footer_right, "%s",
             env && env->hint ? env->hint : "");
    a->page(env, &page);
    ui_render_page(canvas, &page);
}

/* ---- joining the host -------------------------------------------------- */

/* A small generator for the default environment: xorshift32. Not for
 * anything that matters; the device binds esp_random(). */
static uint32_t default_random(void) {
    static uint32_t x = 0x2545F491u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

static applet_env_t  s_default = { .random = default_random };
static applet_env_t *s_env = &s_default;

void applet_bind_env(applet_env_t *env) {
    s_env = env ? env : &s_default;
    if (!s_env->random) {
        s_env->random = default_random;   /* the one service never NULL */
    }
}

applet_env_t *applet_env(void) { return s_env; }

void applet_add_to_shell(ui_shell_t *s) {
    for (uint8_t i = 0; i < applet_count(); i++) {
        const applet_t *a = applet_at(i);
        shell_add_entry(s, (app_mode_t)(APP_MODE_APPLET_FIRST + i), a->name,
                        a->requires, a->absent_hint, a->needs_game);
    }
}

/* The adapters: one set serves every applet, told which by the handler's
 * ctx. */
static bool adapter_enter(void *ctx, cg_game_t game, bool has_game) {
    const applet_t *a = ctx;
    s_env->game     = game;
    s_env->has_game = has_game;
    return a->enter ? a->enter(s_env) : true;
}

static void adapter_exit(void *ctx) {
    const applet_t *a = ctx;
    if (a->exit) {
        a->exit();
    }
}

static void adapter_tick(void *ctx) {
    const applet_t *a = ctx;
    if (a->tick) {
        a->tick(s_env);
    }
}

size_t applet_handlers(app_mode_handler_t *out, size_t cap) {
    size_t n = 0;
    for (uint8_t i = 0; i < applet_count() && n < cap; i++) {
        const applet_t *a = applet_at(i);
        out[n++] = (app_mode_handler_t){
            .mode       = (app_mode_t)(APP_MODE_APPLET_FIRST + i),
            .enter      = adapter_enter,
            .exit       = adapter_exit,
            .tick       = adapter_tick,
            .ctx        = (void *)(uintptr_t)a,   /* the adapters take it const */
            .requires   = a->requires,
            .needs_game = a->needs_game,
        };
    }
    return n;
}

const applet_t *applet_for_mode(app_mode_t mode) {
    if (!APP_MODE_IS_APPLET(mode)) {
        return NULL;
    }
    return applet_at((uint8_t)((int)mode - APP_MODE_APPLET_FIRST));
}

bool applet_input_to(app_mode_t mode, applet_input_t in, int32_t value) {
    const applet_t *a = applet_for_mode(mode);
    return a && a->input && a->input(s_env, in, value);
}
