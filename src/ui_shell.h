/*
 * ui_shell.h -- top-level mode picker.
 *
 * The device does several unrelated jobs and only one at a time, so booting
 * straight into the chip screen is wrong. This is the screen before that:
 * choose a mode, and for the gate modes choose which game, since the gate
 * identity and the chip database both depend on it.
 *
 * Modes are gated by hw_caps, so anything the hardware cannot do is absent
 * rather than present-and-broken. That is the opposite of how chip library tabs
 * behave, and deliberately so: a missing Dark library says something about the
 * game, while a missing nRF24 is just a part nobody fitted.
 */

#ifndef UI_SHELL_H
#define UI_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chipgate.h"
#include "hw_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_MODE_CHIP_GATE = 0, /* be a Battle Chip Gate                     */
    APP_MODE_LINK_USB,      /* link partner for mGBA over USB            */
    APP_MODE_LINK_WIRELESS, /* link partner for another board, ESP-NOW   */
    APP_MODE_MULTIBOOT,     /* serve a .mb program to a stock GBA        */
    APP_MODE_CHIP_SCAN,     /* read chips without a console attached     */
    APP_MODE_USB_STORAGE,   /* hand the drive to a PC, and take it back  */
    APP_MODE_INFRARED,      /* Cyber Drive Zoids IR, via the link port   */
    APP_MODE_SELFTEST,      /* run the tester ROM's checks and show them */
    APP_MODE_SETTINGS,
    APP_MODE_DIAGNOSTICS,
    APP_MODE_COUNT
} app_mode_t;

/*
 * Applets (src/applet.h) take mode numbers from here up, one each, so a mode
 * number says which applet as well as that it is one. The built-in modes stay
 * below APP_MODE_COUNT.
 */
#define APP_MODE_APPLET_FIRST 32
#define SHELL_EXTRA_MAX       8     /* applets the menu can list */
#define APP_MODE_IS_APPLET(m) \
    ((int)(m) >= APP_MODE_APPLET_FIRST && \
     (int)(m) <  APP_MODE_APPLET_FIRST + SHELL_EXTRA_MAX)

/*
 * The menu's Link entry. Not a mode: it opens the choice of interface, USB or
 * Wireless, and that choice is the mode. A board with only one of the two
 * skips the choice.
 */
#define SHELL_ITEM_LINK 0x7E

typedef enum {
    SHELL_SCREEN_MODES = 0, /* pick a mode                    */
    SHELL_SCREEN_GAMES,     /* pick a game                    */
    SHELL_SCREEN_RUNNING,   /* handed off to the mode         */
    SHELL_SCREEN_LINK       /* pick the link: USB or Wireless */
} shell_screen_t;

#define SHELL_ITEMS_MAX (APP_MODE_COUNT + SHELL_EXTRA_MAX)

typedef struct {
    uint32_t       caps;
    shell_screen_t screen;

    /* the menu, filtered by caps: built-in modes, Link, then applets */
    const menu_entry_t *modes[SHELL_ITEMS_MAX];
    uint8_t             mode_count;
    uint8_t             mode_cursor;

    /* applets added with shell_add_entry() */
    menu_entry_t extra[SHELL_EXTRA_MAX];
    bool         extra_needs_game[SHELL_EXTRA_MAX];
    uint8_t      extra_count;

    /* game list, only shown for modes that need one */
    cg_game_t games[CG_GAME_COUNT];
    uint8_t   game_count;
    uint8_t   game_cursor;

    /* link interfaces this board has: APP_MODE_LINK_USB, _WIRELESS */
    app_mode_t links[2];
    uint8_t    link_count;
    uint8_t    link_cursor;

    app_mode_t chosen_mode;
    cg_game_t  chosen_game;
    bool       has_game;
    bool       chosen_needs_game;
} ui_shell_t;

/* Build the mode list for the detected hardware. */
void shell_init(ui_shell_t *s, uint32_t caps);

/* Move the cursor on the current screen, wrapping for a rotary encoder. */
void shell_move(ui_shell_t *s, int32_t delta);

/*
 * Confirm the current selection. Advances modes -> games -> running, and
 * returns true once a mode is ready to start. Modes that need no game skip the
 * middle step.
 */
bool shell_select(ui_shell_t *s);

/* Go back one screen. Returns false when already at the top. */
bool shell_back(ui_shell_t *s);

/* Does a built-in mode need a game chosen before it can start? Applets say
 * so themselves (shell_add_entry). */
bool shell_mode_needs_game(app_mode_t mode);

/*
 * Add an applet to the menu, after the built-in modes and before Settings.
 * mode is APP_MODE_APPLET_FIRST + n; label must outlive the shell. Hidden,
 * like a built-in mode, when caps lacks something in requires. Returns false
 * when the menu is full or the mode number is not an applet's.
 */
bool shell_add_entry(ui_shell_t *s, app_mode_t mode, const char *label,
                     uint32_t requires, const char *absent_hint, bool needs_game);

/* The title for the current screen: the menu, the game list, the link
 * choice. A running mode titles itself. */
const char *shell_screen_title(const ui_shell_t *s);

/* The chosen mode's name, applets included. */
const char *shell_chosen_name(const ui_shell_t *s);

/* Label for the current cursor row, for the renderer. */
const char *shell_current_label(const ui_shell_t *s);
uint8_t     shell_item_count(const ui_shell_t *s);
uint8_t     shell_cursor(const ui_shell_t *s);
const char *shell_label_at(const ui_shell_t *s, uint8_t index);

const char *shell_mode_name(app_mode_t mode);

/* The mode table, exposed so a diagnostics screen can list what was hidden. */
const menu_entry_t *shell_all_modes(size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* UI_SHELL_H */
