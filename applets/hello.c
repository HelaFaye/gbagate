/*
 * hello.c -- the smallest useful applet: a page that counts presses.
 *
 * doc/APPLETS.md builds this one step by step. It shows the three things
 * every page applet does: keep its state in statics, change it in input(),
 * and describe it in page(). The host draws the page in the current theme.
 */

#include "applet.h"

static uint32_t s_presses;
static int32_t  s_turned;

static bool hello_enter(const applet_env_t *env) {
    (void)env;
    s_presses = 0;     /* every visit starts fresh */
    s_turned  = 0;
    return true;
}

static bool hello_input(const applet_env_t *env, applet_input_t in, int32_t value) {
    (void)env;
    switch (in) {
    case APPLET_IN_CONFIRM: s_presses++;      return true;
    case APPLET_IN_MOVE:    s_turned += value; return true;
    default:                return false;   /* Back: let the host leave */
    }
}

static void hello_page(const applet_env_t *env, ui_page_t *p) {
    (void)env;
    ui_page_add(p, COL_TEXT, "Hello, gbagate!");
    ui_page_add(p, COL_TEXT_DIM, "pressed %lu times", (unsigned long)s_presses);
    ui_page_add(p, COL_TEXT_DIM, "turned  %ld", (long)s_turned);
}

const applet_t applet_hello = {
    .name     = "Hello",
    .requires = HW_CAP_DISPLAY,
    .enter    = hello_enter,
    .input    = hello_input,
    .page     = hello_page,
};
