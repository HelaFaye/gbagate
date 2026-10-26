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

/*
 * The menu's order. Link stands for both link modes; applets go between Self
 * Test and Settings, so the two entries every board has stay at the bottom
 * where they are always found.
 */
static const menu_entry_t k_link = {
    SHELL_ITEM_LINK, "Link", HW_CAP_DISPLAY | HW_CAP_LINK_PORT,
    "connect the link cable"
};

static const int k_head[] = {
    APP_MODE_CHIP_GATE, SHELL_ITEM_LINK, APP_MODE_MULTIBOOT, APP_MODE_CHIP_SCAN,
    APP_MODE_INFRARED, APP_MODE_USB_STORAGE, APP_MODE_SELFTEST,
};
static const int k_tail[] = { APP_MODE_SETTINGS, APP_MODE_DIAGNOSTICS };

static void add_visible(ui_shell_t *s, const menu_entry_t *e) {
    if (s->mode_count < SHELL_ITEMS_MAX && hw_menu_missing(e, s->caps) == 0) {
        s->modes[s->mode_count++] = e;
    }
}

static void rebuild(ui_shell_t *s) {
    s->mode_count = 0;
    for (size_t i = 0; i < sizeof k_head / sizeof k_head[0]; i++) {
        if (k_head[i] == SHELL_ITEM_LINK) {
            /* Listed only when there is an interface to choose. */
            if (s->link_count > 0) {
                add_visible(s, &k_link);
            }
        } else {
            add_visible(s, &k_modes[k_head[i]]);
        }
    }
    for (uint8_t i = 0; i < s->extra_count; i++) {
        add_visible(s, &s->extra[i]);
    }
    for (size_t i = 0; i < sizeof k_tail / sizeof k_tail[0]; i++) {
        add_visible(s, &k_modes[k_tail[i]]);
    }
    if (s->mode_cursor >= s->mode_count) {
        s->mode_cursor = 0;
    }
}

void shell_init(ui_shell_t *s, uint32_t caps) {
    memset(s, 0, sizeof *s);
    s->caps   = caps;
    s->screen = SHELL_SCREEN_MODES;

    const app_mode_t links[] = { APP_MODE_LINK_USB, APP_MODE_LINK_WIRELESS };
    for (size_t i = 0; i < sizeof links / sizeof links[0]; i++) {
        if (hw_menu_missing(&k_modes[links[i]], caps) == 0) {
            s->links[s->link_count++] = links[i];
        }
    }
    for (int g = 0; g < CG_GAME_COUNT; g++) {
        s->games[s->game_count++] = (cg_game_t)g;
    }
    rebuild(s);
}

bool shell_add_entry(ui_shell_t *s, app_mode_t mode, const char *label,
                     uint32_t requires, const char *absent_hint, bool needs_game) {
    if (!APP_MODE_IS_APPLET(mode) || !label || s->extra_count >= SHELL_EXTRA_MAX) {
        return false;
    }
    for (uint8_t i = 0; i < s->extra_count; i++) {
        if (s->extra[i].id == (int)mode) {
            return false;   /* one entry per mode */
        }
    }
    menu_entry_t *e = &s->extra[s->extra_count];
    e->id          = (int)mode;
    e->label       = label;
    e->requires    = requires;
    e->absent_hint = absent_hint;
    s->extra_needs_game[s->extra_count] = needs_game;
    s->extra_count++;
    rebuild(s);
    return true;
}

static const menu_entry_t *extra_for(const ui_shell_t *s, app_mode_t mode,
                                     bool *needs_game) {
    for (uint8_t i = 0; i < s->extra_count; i++) {
        if (s->extra[i].id == (int)mode) {
            if (needs_game) *needs_game = s->extra_needs_game[i];
            return &s->extra[i];
        }
    }
    return NULL;
}

/* The link choice: the interface, then what it connects to. */
static const char *link_label(app_mode_t m) {
    return m == APP_MODE_LINK_USB ? "USB - PC or mGBA" : "Wireless - gbagate";
}

static uint8_t *cursor_of(ui_shell_t *s) {
    switch (s->screen) {
    case SHELL_SCREEN_GAMES: return &s->game_cursor;
    case SHELL_SCREEN_LINK:  return &s->link_cursor;
    default:                 return &s->mode_cursor;
    }
}

uint8_t shell_item_count(const ui_shell_t *s) {
    switch (s->screen) {
    case SHELL_SCREEN_MODES: return s->mode_count;
    case SHELL_SCREEN_GAMES: return s->game_count;
    case SHELL_SCREEN_LINK:  return s->link_count;
    default:                 return 0;
    }
}

uint8_t shell_cursor(const ui_shell_t *s) {
    return *cursor_of((ui_shell_t *)s);
}

const char *shell_label_at(const ui_shell_t *s, uint8_t index) {
    switch (s->screen) {
    case SHELL_SCREEN_MODES:
        return index < s->mode_count ? s->modes[index]->label : NULL;
    case SHELL_SCREEN_GAMES:
        return index < s->game_count ? chipgate_game_name(s->games[index]) : NULL;
    case SHELL_SCREEN_LINK:
        return index < s->link_count ? link_label(s->links[index]) : NULL;
    default:
        return NULL;
    }
}

const char *shell_current_label(const ui_shell_t *s) {
    return shell_label_at(s, shell_cursor(s));
}

const char *shell_screen_title(const ui_shell_t *s) {
    switch (s->screen) {
    case SHELL_SCREEN_GAMES: return "Choose game";
    case SHELL_SCREEN_LINK:  return "Link over";
    default:                 return "gbagate";
    }
}

void shell_move(ui_shell_t *s, int32_t delta) {
    uint8_t n = shell_item_count(s);
    if (n == 0 || delta == 0) {
        return;
    }

    uint8_t *cursor = cursor_of(s);
    int32_t pos = ((int32_t)*cursor + delta) % (int32_t)n;
    if (pos < 0) {
        pos += n;
    }
    *cursor = (uint8_t)pos;
}

/* A mode is chosen: ask for a game if it needs one, else run it. */
static bool choose(ui_shell_t *s, app_mode_t mode, bool needs_game) {
    s->chosen_mode       = mode;
    s->chosen_needs_game = needs_game;
    if (needs_game) {
        s->screen = SHELL_SCREEN_GAMES;
        return false;
    }
    s->has_game = false;
    s->screen   = SHELL_SCREEN_RUNNING;
    return true;
}

bool shell_select(ui_shell_t *s) {
    switch (s->screen) {
    case SHELL_SCREEN_MODES: {
        if (s->mode_cursor >= s->mode_count) {
            return false;
        }
        int id = s->modes[s->mode_cursor]->id;
        if (id == SHELL_ITEM_LINK) {
            /* One interface: no choice to make. */
            if (s->link_count == 1) {
                return choose(s, s->links[0], false);
            }
            s->screen = SHELL_SCREEN_LINK;
            return false;
        }
        bool needs = false;
        if (APP_MODE_IS_APPLET(id)) {
            extra_for(s, (app_mode_t)id, &needs);
        } else {
            needs = shell_mode_needs_game((app_mode_t)id);
        }
        return choose(s, (app_mode_t)id, needs);
    }

    case SHELL_SCREEN_LINK:
        if (s->link_cursor >= s->link_count) {
            return false;
        }
        return choose(s, s->links[s->link_cursor], false);

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

static bool is_link(app_mode_t m) {
    return m == APP_MODE_LINK_USB || m == APP_MODE_LINK_WIRELESS;
}

bool shell_back(ui_shell_t *s) {
    switch (s->screen) {
    case SHELL_SCREEN_RUNNING:
        /* Returning from a mode lands where it was chosen, not at the top:
         * switching games, or link, is the common reason to come back. */
        if (s->chosen_needs_game) {
            s->screen = SHELL_SCREEN_GAMES;
        } else if (is_link(s->chosen_mode) && s->link_count > 1) {
            s->screen = SHELL_SCREEN_LINK;
        } else {
            s->screen = SHELL_SCREEN_MODES;
        }
        return true;
    case SHELL_SCREEN_GAMES:
    case SHELL_SCREEN_LINK:
        s->screen = SHELL_SCREEN_MODES;
        return true;
    default:
        return false;
    }
}

const char *shell_chosen_name(const ui_shell_t *s) {
    const menu_entry_t *e = extra_for(s, s->chosen_mode, NULL);
    return e ? e->label : shell_mode_name(s->chosen_mode);
}

const char *shell_mode_name(app_mode_t mode) {
    if (mode >= APP_MODE_COUNT) {
        return "?";
    }
    return k_modes[mode].label;
}
