/*
 * gba_memory.c -- read and write a GBA's memory from the board.
 *
 * Sends the memory agent (rom/agent.c) to a GBA waiting at its boot screen,
 * then offers three things to try on it:
 *
 *   Ping        is the agent still answering?
 *   Colour      write the backdrop colour (palette 0, a POKE16): the GBA's
 *               screen changes, the plainest proof a memory write landed
 *   Test 4 KB   write a pattern to free EWRAM, read it back, compare
 *
 * The example of an applet using the agent services (applet.h): it starts the
 * agent from tick() a frame after confirm, so "sending..." is on screen while
 * the slow part runs, and gives the link port back in exit().
 */

#include <stdio.h>
#include <string.h>

#include "applet.h"

enum { ACT_PING, ACT_COLOUR, ACT_TEST, ACT_COUNT };
static const char *const k_actions[ACT_COUNT] = { "Ping", "Colour", "Test 4 KB" };

static const struct { const char *name; uint16_t bgr555; } k_colours[] = {
    { "red", 0x001F }, { "green", 0x03E0 }, { "blue", 0x7C00 },
    { "yellow", 0x03FF }, { "white", 0x7FFF }, { "black", 0x0000 },
};
#define N_COLOURS (sizeof k_colours / sizeof k_colours[0])

static bool     s_pending;     /* confirm pressed: start on the next tick */
static bool     s_running;     /* the agent answered                      */
static uint8_t  s_cursor;
static uint8_t  s_colour;
static uint32_t s_tests;
static char     s_result[40];
static char     s_error[40];

static bool mem_enter(const applet_env_t *env) {
    s_pending = s_running = false;
    s_cursor = 0;
    s_colour = 0;
    s_tests = 0;
    s_result[0] = s_error[0] = '\0';
    return env->agent_start && env->agent;   /* no link port, no applet */
}

static void mem_exit(void) {
    const applet_env_t *env = applet_env();
    if (env->agent_stop) {
        env->agent_stop();
    }
    s_running = false;
}

static void mem_tick(const applet_env_t *env) {
    if (!s_pending) {
        return;
    }
    s_pending = false;
    const char *why = env->agent_start();
    s_running = why == NULL;
    snprintf(s_error, sizeof s_error, "%s", why ? why : "");
    s_result[0] = '\0';
}

static void say(mba_result_t r, const char *ok) {
    if (r == MBA_OK) {
        snprintf(s_result, sizeof s_result, "%s", ok);
    } else {
        snprintf(s_result, sizeof s_result, "%s", mba_result_name(r));
    }
}

static void act(const applet_env_t *env) {
    mba_client_t *c = env->agent();
    if (!c) {
        s_running = false;
        return;
    }
    switch (s_cursor) {
    case ACT_PING:
        say(mba_ping(c), "the agent answers");
        break;
    case ACT_COLOUR: {
        s_colour = (uint8_t)((s_colour + 1) % N_COLOURS);
        char ok[40];
        snprintf(ok, sizeof ok, "backdrop: %s", k_colours[s_colour].name);
        say(mba_poke16(c, 0x05000000, k_colours[s_colour].bgr555), ok);
        break;
    }
    case ACT_TEST: {
        /* A fresh pattern each time, so a stale read cannot pass. */
        static uint32_t out[1024], back[1024];
        uint32_t seed = env->random();
        for (int i = 0; i < 1024; i++) {
            seed = seed * 1664525u + 1013904223u;
            out[i] = seed;
        }
        uint32_t at = (c->free_start + 0xFFFu) & ~0xFFFu;   /* clear of the agent */
        mba_result_t r = mba_write(c, at, out, 1024);
        if (r == MBA_OK) {
            memset(back, 0, sizeof back);
            r = mba_read(c, at, back, 1024);
        }
        if (r == MBA_OK && memcmp(out, back, sizeof out) != 0) {
            snprintf(s_result, sizeof s_result, "read back differs");
        } else {
            s_tests += r == MBA_OK;
            char ok[40];
            snprintf(ok, sizeof ok, "4 KB ok (%lu)", (unsigned long)s_tests);
            say(r, ok);
        }
        break;
    }
    default:
        break;
    }
}

static bool mem_input(const applet_env_t *env, applet_input_t in, int32_t value) {
    switch (in) {
    case APPLET_IN_CONFIRM:
        if (s_running) {
            act(env);
        } else {
            s_pending = true;   /* tick() sends, once "sending..." is drawn */
            s_error[0] = '\0';
        }
        return true;
    case APPLET_IN_MOVE:
        if (s_running && value) {
            int n = ((int)s_cursor + (value > 0 ? 1 : -1) + ACT_COUNT) % ACT_COUNT;
            s_cursor = (uint8_t)n;
        }
        return true;
    default:
        return false;   /* Back leaves */
    }
}

/* The host has the page started: title, and Back on the footer's right. */
static void mem_page(const applet_env_t *env, ui_page_t *p) {
    const char *key = env->confirm ? env->confirm : "A";
    if (s_pending) {
        ui_page_add(p, COL_WARN, "sending the agent...");
    } else if (!s_running) {
        if (s_error[0]) {
            ui_page_add(p, COL_BAD, "%s", s_error);
        }
        ui_page_add(p, COL_TEXT, "turn on a GBA with");
        ui_page_add(p, COL_TEXT, "no cartridge, at its");
        ui_page_add(p, COL_TEXT, "boot screen; link it");
        snprintf(p->footer, sizeof p->footer, "%s send", key);
    } else {
        mba_client_t *c = env->agent();
        ui_page_add(p, COL_GOOD, "agent v%lu running", (unsigned long)(c ? c->version : 0));
        ui_page_add(p, COL_TEXT_DIM, "free from %08lX", (unsigned long)(c ? c->free_start : 0));
        for (int i = 0; i < ACT_COUNT; i++) {
            ui_page_add(p, i == s_cursor ? COL_CURSOR : COL_TEXT, "%c %s",
                        i == s_cursor ? '>' : ' ', k_actions[i]);
        }
        if (s_result[0]) {
            ui_page_add(p, strstr(s_result, "ok") || strstr(s_result, "answers") ||
                           strstr(s_result, "backdrop") ? COL_GOOD : COL_BAD, "%s", s_result);
        }
        snprintf(p->footer, sizeof p->footer, "%s do it", key);
    }
}

const applet_t applet_gba_memory = {
    .name        = "GBA Memory",
    .requires    = HW_CAP_DISPLAY | HW_CAP_LINK_PORT,
    .absent_hint = "connect the link cable",
    .enter       = mem_enter,
    .exit        = mem_exit,
    .tick        = mem_tick,
    .input       = mem_input,
    .page        = mem_page,
};
