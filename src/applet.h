/*
 * applet.h -- add a mode to the device without touching the firmware's core.
 *
 * An applet is a struct: a name for the menu, the hardware it needs, and a
 * handful of callbacks. List it in applets/applets.def and it appears in the
 * main menu, is started and stopped by the same dispatcher as the built-in
 * modes, gets the same buttons and draws with the same renderer and theme.
 * doc/APPLETS.md walks through writing one; applets/ holds three examples.
 *
 * What an applet may rely on:
 *
 *   - enter() runs once when it is chosen. Return false to refuse (the menu
 *     then says why it did not start). exit() runs when it is left by any
 *     route, refused or not, so free what enter() took there.
 *   - tick() runs every UI iteration while it is up, about 60 times a second.
 *   - input() gets one button intent at a time. Return true if it was used.
 *     APPLET_IN_BACK that is not used leaves the applet, so Back always works
 *     unless the applet has something of its own to cancel first.
 *   - Exactly one of page() and draw() draws it. page() fills a ui_page_t --
 *     title, lines, footer -- which the host renders in the current theme;
 *     draw() gets the whole 128x128 canvas. Both are called every frame, and
 *     must not block.
 *   - env says what the host offers. Treat every service pointer in it as
 *     optional: an image without chip databases passes open_db as NULL.
 *
 * Applets run on the UI task. Anything slow -- a flash write, a long
 * transfer -- belongs in a service the host provides, not in a callback.
 */

#ifndef APPLET_H
#define APPLET_H

#include <stdbool.h>
#include <stdint.h>

#include "app_modes.h"
#include "chipdb.h"
#include "chipgate.h"
#include "hw_caps.h"
#include "ui_screens.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APPLET_MAX 8

typedef enum {
    APPLET_IN_MOVE = 0,   /* value: detents or steps, signed               */
    APPLET_IN_CONFIRM,    /* the encoder button, or A                      */
    APPLET_IN_BACK,       /* hold, or B                                    */
} applet_input_t;

typedef struct {
    uint32_t    now_ms;
    uint32_t    caps;       /* hw_caps of this board                        */
    cg_game_t   game;       /* meaningful only when has_game                */
    bool        has_game;
    const char *hint;       /* the control hint, for a page's footer        */

    /* Open the chip database for a game into *db. NULL when the image has
     * none. The blob lives in flash: the chipdb_t can be kept. */
    bool     (*open_db)(cg_game_t game, chipdb_t *db);
    /* 32 random bits. Never NULL. */
    uint32_t (*random)(void);
} applet_env_t;

typedef struct applet {
    const char *name;         /* menu label and page title                  */
    uint32_t    requires;     /* hw_caps bits, all needed to be listed      */
    const char *absent_hint;  /* why it is hidden, for Diagnostics          */
    bool        needs_game;   /* ask which game first, like Battle Chip     */

    bool (*enter)(const applet_env_t *env);
    void (*exit)(void);
    void (*tick)(const applet_env_t *env);
    bool (*input)(const applet_env_t *env, applet_input_t in, int32_t value);
    void (*page)(const applet_env_t *env, ui_page_t *page);
    void (*draw)(const applet_env_t *env, const ui_canvas_t *canvas);
} applet_t;

/* Every applet applets/applets.def names, in its order. */
uint8_t         applet_count(void);
const applet_t *applet_at(uint8_t index);

/*
 * Is this applet usable? A name, and exactly one of page() and draw(). The
 * registry skips one that is not, rather than listing a mode that would
 * draw nothing -- the fault this whole layer exists to prevent.
 */
bool applet_valid(const applet_t *a);

/* Draw the applet the way the host does: page() through the renderer, or
 * draw() straight onto the canvas. */
void applet_render(const applet_t *a, const applet_env_t *env,
                   const ui_canvas_t *canvas);

/* ---- joining the host --------------------------------------------------
 *
 * What the firmware calls; an applet never needs these. The applet with
 * index n runs as mode APP_MODE_APPLET_FIRST + n.
 */

/*
 * The environment every callback receives. The host owns it and keeps it
 * current (now_ms, hint) before each frame; enter() fills in the game. Until
 * one is bound, a built-in default with no services stands in, so no applet
 * ever sees NULL.
 */
void          applet_bind_env(applet_env_t *env);
applet_env_t *applet_env(void);

/* List every applet in the menu, before Settings. */
void applet_add_to_shell(ui_shell_t *s);

/* A dispatcher handler per applet, into out[]. Returns how many. */
size_t applet_handlers(app_mode_handler_t *out, size_t cap);

/* The applet a mode runs, or NULL if the mode is not an applet's. */
const applet_t *applet_for_mode(app_mode_t mode);

/*
 * Hand a button to the running applet. True if it used it. A Back it does
 * not use is the host's cue to leave.
 */
bool applet_input_to(app_mode_t mode, applet_input_t in, int32_t value);

#ifdef __cplusplus
}
#endif

#endif /* APPLET_H */
