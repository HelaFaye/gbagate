/*
 * ui_shell.c -- see ui_shell.h
 */

#include <string.h>

#include "ui_shell.h"

/*
 * Every mode declares the hardware it needs. Diagnostics requires nothing, on
 * purpose: when something is missing, the screen that explains why must itself
 * always be reachable.
 */
static const menu_entry_t k_modes[APP_MODE_COUNT] = {
    { APP_MODE_CHIP_GATE, "Battle Chip",
      HW_CAP_DISPLAY | HW_CAP_LINK_PORT, "connect the link cable" },
    { APP_MODE_LINK_USB, "Link: USB",
      HW_CAP_DISPLAY | HW_CAP_LINK_PORT | HW_CAP_USB, "plug in USB" },
    { APP_MODE_LINK_WIRELESS, "Link: Wireless",
      HW_CAP_DISPLAY | HW_CAP_LINK_PORT | HW_CAP_WIFI, "WiFi unavailable" },
    { APP_MODE_MULTIBOOT, "Send Program",
      HW_CAP_DISPLAY | HW_CAP_LINK_PORT, "connect the link cable" },
    { APP_MODE_CHIP_SCAN, "Read Chip",
      HW_CAP_DISPLAY | HW_CAP_CHIP_READER, "reader board not detected" },
    /*
     * Infrared drives an LED from RCNT bit 3 in General Purpose mode, so it
     * needs the link port and nothing else -- no SIO, no clock. ir_cdz.c has
     * had the protocol since the Cyber Drive Zoids article; without an entry
     * here there was no way to reach it.
     */
    { APP_MODE_INFRARED, "Infrared",
      HW_CAP_DISPLAY | HW_CAP_LINK_PORT, "connect the link cable" },

    /*
     * Handing the volume over is a deliberate action, not something to infer
     * from what the host appears to be doing. A device that cannot be told to
     * release its storage is one you end up power-cycling.
     */
    { APP_MODE_USB_STORAGE, "USB Storage",
      HW_CAP_DISPLAY | HW_CAP_USB, "plug in USB" },

    /*
     * Self-test drives the link itself, against rom/tester.gba on a console or
     * a multiboot image it sends first. It needs the link port and a screen to
     * report on, and nothing else -- deliberately, because it is the mode you
     * reach for when you are not sure what else works.
     */
    { APP_MODE_SELFTEST, "Self Test",
      HW_CAP_DISPLAY | HW_CAP_LINK_PORT, "connect the link cable" },
    { APP_MODE_SETTINGS, "Settings", HW_CAP_DISPLAY, NULL },
    { APP_MODE_DIAGNOSTICS, "Diagnostics", 0, NULL },
};

const menu_entry_t *shell_all_modes(size_t *count) {
    if (count) {
        *count = APP_MODE_COUNT;
    }
    return k_modes;
}

bool shell_mode_needs_game(app_mode_t mode) {
    /* The gate identity and the chip database are both per-game, so anything
     * that impersonates a gate has to know which one. */
    /*
     * Self-test needs one too: it checks the gate identity, and which identity
     * is correct depends on the game.
     */
    return mode == APP_MODE_CHIP_GATE || mode == APP_MODE_CHIP_SCAN ||
           mode == APP_MODE_SELFTEST;
}

void shell_init(ui_shell_t *s, uint32_t caps) {
    memset(s, 0, sizeof *s);
    s->caps   = caps;
    s->screen = SHELL_SCREEN_MODES;

    s->mode_count = (uint8_t)hw_menu_filter(k_modes, APP_MODE_COUNT, caps,
                                            s->modes, APP_MODE_COUNT);

    for (int g = 0; g < CG_GAME_COUNT; g++) {
        s->games[s->game_count++] = (cg_game_t)g;
    }
}

uint8_t shell_item_count(const ui_shell_t *s) {
    switch (s->screen) {
    case SHELL_SCREEN_MODES: return s->mode_count;
    case SHELL_SCREEN_GAMES: return s->game_count;
    default:                 return 0;
    }
}

uint8_t shell_cursor(const ui_shell_t *s) {
    return s->screen == SHELL_SCREEN_GAMES ? s->game_cursor : s->mode_cursor;
}

const char *shell_label_at(const ui_shell_t *s, uint8_t index) {
    switch (s->screen) {
    case SHELL_SCREEN_MODES:
        return index < s->mode_count ? s->modes[index]->label : NULL;
    case SHELL_SCREEN_GAMES:
        return index < s->game_count ? chipgate_game_name(s->games[index]) : NULL;
    default:
        return NULL;
    }
}

const char *shell_current_label(const ui_shell_t *s) {
    return shell_label_at(s, shell_cursor(s));
}

void shell_move(ui_shell_t *s, int32_t delta) {
    uint8_t n = shell_item_count(s);
    if (n == 0 || delta == 0) {
        return;
    }

    uint8_t *cursor = (s->screen == SHELL_SCREEN_GAMES) ? &s->game_cursor
                                                        : &s->mode_cursor;
    int32_t pos = ((int32_t)*cursor + delta) % (int32_t)n;
    if (pos < 0) {
        pos += n;
    }
    *cursor = (uint8_t)pos;
}

bool shell_select(ui_shell_t *s) {
    switch (s->screen) {
    case SHELL_SCREEN_MODES: {
        if (s->mode_cursor >= s->mode_count) {
            return false;
        }
        s->chosen_mode = (app_mode_t)s->modes[s->mode_cursor]->id;
        if (shell_mode_needs_game(s->chosen_mode)) {
            s->screen = SHELL_SCREEN_GAMES;
            return false;
        }
        s->has_game = false;
        s->screen   = SHELL_SCREEN_RUNNING;
        return true;
    }

    case SHELL_SCREEN_GAMES:
        if (s->game_cursor >= s->game_count) {
            return false;
        }
        s->chosen_game = s->games[s->game_cursor];
        s->has_game    = true;
        s->screen      = SHELL_SCREEN_RUNNING;
        return true;

    default:
        return false;
    }
}

bool shell_back(ui_shell_t *s) {
    switch (s->screen) {
    case SHELL_SCREEN_RUNNING:
        /* Returning from a mode lands where it was chosen, not at the top:
         * switching games is the common reason to come back. */
        s->screen = shell_mode_needs_game(s->chosen_mode) ? SHELL_SCREEN_GAMES
                                                          : SHELL_SCREEN_MODES;
        return true;
    case SHELL_SCREEN_GAMES:
        s->screen = SHELL_SCREEN_MODES;
        return true;
    default:
        return false;
    }
}

const char *shell_mode_name(app_mode_t mode) {
    if (mode >= APP_MODE_COUNT) {
        return "?";
    }
    return k_modes[mode].label;
}
