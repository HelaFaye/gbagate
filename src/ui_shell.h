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
    APP_MODE_SETTINGS,
    APP_MODE_DIAGNOSTICS,
    APP_MODE_COUNT
} app_mode_t;

typedef enum {
    SHELL_SCREEN_MODES = 0, /* pick a mode           */
    SHELL_SCREEN_GAMES,     /* pick a game           */
    SHELL_SCREEN_RUNNING    /* handed off to the mode */
} shell_screen_t;

typedef struct {
    uint32_t       caps;
    shell_screen_t screen;

    /* mode list, filtered by caps */
    const menu_entry_t *modes[APP_MODE_COUNT];
    uint8_t             mode_count;
    uint8_t             mode_cursor;

    /* game list, only shown for modes that need one */
    cg_game_t games[CG_GAME_COUNT];
    uint8_t   game_count;
    uint8_t   game_cursor;

    app_mode_t chosen_mode;
    cg_game_t  chosen_game;
    bool       has_game;
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

/* Does a mode need a game chosen before it can start? */
bool shell_mode_needs_game(app_mode_t mode);

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
