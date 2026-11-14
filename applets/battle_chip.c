/*
 * battle_chip.c -- be a Battle Chip Gate: choose a chip and slot it in, or
 * put a real one in the reader.
 *
 * This was two built-in modes, Battle Chip and Read Chip. They are one job --
 * get a chip into the game -- and a real gate does both at once: a chip in
 * the slot is a chip sent. So here the list and the reader work together.
 * Choosing a chip sends it, and with a reader board fitted, inserting a
 * physical chip sends that one and moves the list to it.
 *
 * The heaviest applet: it uses the gate, the reader and the favourites
 * through applet_env_t, and draws the chip screen (ui_render.h) on the
 * whole canvas. Everything it needs from the device is a service, so it
 * runs unchanged in test_applets against fakes.
 *
 * Controls: turn or up/down to choose, click or A to send (in Move mode: pick
 * up and place), left/right for the library tabs, Select to mark a
 * favourite, Start (or a hold on A) to cycle the sort -- the last of which is
 * Move, for arranging the list by hand. Back cancels a chip still going out,
 * then leaves.
 */

#include <stdio.h>
#include <string.h>

#include "applet.h"
#include "ui_chipview.h"
#include "ui_list.h"

#define READ_EVERY_MS 250   /* the reader's pads, a few times a second */

static chipdb_t   s_db;
static chipview_t s_view;
static ui_list_t  s_list;
static uint16_t   s_storage[CHIPVIEW_MAX_RECORDS];
static char       s_status[32];
static bool       s_linked;
static uint16_t   s_in_slot;        /* the reader's chip, CG_CHIP_NONE if none */
static uint32_t   s_last_read_ms;

static void say(const char *fmt, const char *arg) {
    snprintf(s_status, sizeof s_status, fmt, arg);
}

static void relist(void) {
    chipview_rebuild(&s_view);
    ui_list_set(&s_list, s_view.view, s_view.view_count);
}

static bool bc_enter(const applet_env_t *env) {
    if (!env->has_game || !env->open_db || !env->open_db(env->game, &s_db)) {
        return false;   /* no database for this game: nothing to choose from */
    }
    if (!env->gate_start || !env->gate_start(env->game)) {
        return false;   /* no link port: nothing to send through */
    }
    chipview_init(&s_view, &s_db, s_storage, CHIPVIEW_MAX_RECORDS);
    /* Favourites survive power cycles in the host's settings. */
    if (env->favorites) {
        size_t n = env->favorites_len < sizeof s_view.favorites
                       ? env->favorites_len : sizeof s_view.favorites;
        memcpy(s_view.favorites, env->favorites, n);
    }
    ui_list_init(&s_list, UI_ROWS, true);
    relist();
    s_linked  = false;
    s_in_slot = CG_CHIP_NONE;
    s_last_read_ms = 0;
    say("%s", "WAITING");
    return true;
}

static void save_favorites(const applet_env_t *env) {
    if (!s_view.favorites_dirty || !env->favorites) {
        return;
    }
    size_t n = env->favorites_len < sizeof s_view.favorites
                   ? env->favorites_len : sizeof s_view.favorites;
    memcpy(env->favorites, s_view.favorites, n);
    s_view.favorites_dirty = false;
    if (env->favorites_changed) {
        env->favorites_changed();
    }
}

static void bc_exit(void) {
    applet_env_t *env = applet_env();
    /* A chip staged against a link that never completed would leave the gate
     * busy for ever; drop it on the way out, and save the stars. */
    if (env->gate_cancel) {
        env->gate_cancel();
    }
    save_favorites(env);
    if (env->gate_stop) {
        env->gate_stop();
    }
}

/* Send the chip in record slot `slot`, and say so. */
static void send_slot(const applet_env_t *env, uint16_t slot, const char *how) {
    uint16_t id = chipdb_id_for_slot(&s_db, slot);
    if (id == CHIPDB_NO_ID) {
        /* No wire ID: the game has no number for it on a gate (104 of BN4's
         * chips). Saying so beats a press that does nothing. */
        say("%s", "NOT A GATE CHIP");
        return;
    }
    if (!env->gate_send(id)) {
        say("%s", "BUSY");
        return;
    }
    char what[32];
    snprintf(what, sizeof what, "%s %s", how, chipdb_name(&s_db, slot));
    say("%s", what);
}

/* Put the cursor on a record slot, if the current tab shows it. */
static void show_slot(uint16_t slot) {
    for (uint16_t i = 0; i < s_view.view_count; i++) {
        if (s_view.view[i] == slot) {
            ui_list_move(&s_list, (int32_t)i - (int32_t)s_list.cursor);
            return;
        }
    }
}

static void bc_tick(const applet_env_t *env) {
    bool linked = env->gate_linked && env->gate_linked();
    if (linked != s_linked) {
        s_linked = linked;
        say("%s", linked ? "LINKED" : "WAITING");
    }
    /*
     * The reader: a chip that arrives in the slot is sent, as a real gate
     * sends it, and the list goes to it so the screen shows what went. It is
     * sent once per insertion; take it out to send it again.
     */
    if (!env->read_chip || (uint32_t)(env->now_ms - s_last_read_ms) < READ_EVERY_MS) {
        return;
    }
    s_last_read_ms = env->now_ms;
    uint16_t id = env->read_chip();
    if (id == s_in_slot) {
        return;
    }
    s_in_slot = id;
    if (id == CG_CHIP_NONE) {
        return;
    }
    uint16_t slot = chipdb_slot_for_id(&s_db, id);
    if (slot == CHIPDB_NO_RECORD) {
        say("%s", "UNKNOWN CHIP");
        return;
    }
    show_slot(slot);
    send_slot(env, slot, "READ");
}

static bool bc_input(const applet_env_t *env, applet_input_t in, int32_t value) {
    uint16_t pos = 0;
    bool has = ui_list_selected(&s_list, &pos);
    uint16_t slot = has ? chipview_slot_at(&s_view, s_list.cursor) : CHIPVIEW_NONE;

    switch (in) {
    case APPLET_IN_MOVE:
        ui_list_move(&s_list, value);
        return true;

    case APPLET_IN_CONFIRM:
        if (!has) {
            return true;
        }
        if (s_view.mode == CHIP_MODE_MOVE) {
            if (chipview_held(&s_view) == CHIPVIEW_NONE) {
                chipview_pick(&s_view, s_list.cursor);
            } else if (chipview_place(&s_view, s_list.cursor)) {
                ui_list_set(&s_list, s_view.view, s_view.view_count);
            }
            return true;
        }
        send_slot(env, slot, "SENT");
        return true;

    case APPLET_IN_BACK:
        /* Things to undo first: a chip held in Move mode, a chip going out.
         * With neither, Back is the host's: leave. */
        if (chipview_held(&s_view) != CHIPVIEW_NONE) {
            chipview_cancel(&s_view);
            return true;
        }
        if (env->gate_cancel && env->gate_cancel()) {
            say("%s", "CANCELLED");
            return true;
        }
        return false;

    case APPLET_IN_TAB:
        chipview_tab_cycle(&s_view, value < 0 ? -1 : 1);
        ui_list_set(&s_list, s_view.view, s_view.view_count);
        return true;

    case APPLET_IN_MARK:
        if (slot != CHIPVIEW_NONE) {
            chipview_toggle_favorite(&s_view, slot);
            ui_list_set(&s_list, s_view.view, s_view.view_count);
            save_favorites(env);
        }
        return true;

    case APPLET_IN_MENU: {
        /* Library, name, attack, element, then Move: arrange by hand, which
         * leaves the custom order behind as the fifth sort. */
        if (s_view.mode == CHIP_MODE_MOVE) {
            chipview_cancel(&s_view);
            chipview_set_mode(&s_view, CHIP_MODE_SLOT_IN);
            chipview_set_sort(&s_view, CHIP_SORT_LIBRARY);
        } else if (s_view.sort + 1 >= CHIP_SORT_CUSTOM) {
            chipview_set_mode(&s_view, CHIP_MODE_MOVE);
        } else {
            chipview_set_sort(&s_view, (chip_sort_t)(s_view.sort + 1));
        }
        ui_list_set(&s_list, s_view.view, s_view.view_count);
        say("%s", s_view.mode == CHIP_MODE_MOVE ? "MOVE"
                                                : chipview_sort_name(s_view.sort));
        return true;
    }
    }
    return false;
}

static void bc_draw(const applet_env_t *env, const ui_canvas_t *c) {
    (void)env;
    /* The list must stay backed by this view or the renderer refuses to
     * draw it (ui_render_list_matches()). */
    if (s_list.items != s_view.view || s_list.count != s_view.view_count) {
        ui_list_set(&s_list, s_view.view, s_view.view_count);
    }
    ui_render_chip_screen(c, &s_view, &s_db, &s_list, s_status);
}

/* The record slot on a row of the current list, or CHIPVIEW_NONE: what a
 * test or a diagnostics page needs to know which chip a row is. */
uint16_t battle_chip_row_slot(uint16_t row) {
    return row < s_view.view_count ? chipview_slot_at(&s_view, row) : CHIPVIEW_NONE;
}

const applet_t applet_battle_chip = {
    .name        = "Battle Chip",
    .requires    = HW_CAP_DISPLAY | HW_CAP_LINK_PORT,
    .absent_hint = "connect the link cable",
    .needs_game  = true,
    .controls    = CTRL_BIT(CTRL_PREV) | CTRL_BIT(CTRL_NEXT) |
                   CTRL_BIT(CTRL_PAGE_PREV) | CTRL_BIT(CTRL_PAGE_NEXT) |
                   CTRL_BIT(CTRL_CONFIRM) | CTRL_BIT(CTRL_CANCEL) |
                   CTRL_BIT(CTRL_TAB_PREV) | CTRL_BIT(CTRL_TAB_NEXT) |
                   CTRL_BIT(CTRL_MARK) | CTRL_BIT(CTRL_MENU),
    .enter       = bc_enter,
    .exit        = bc_exit,
    .tick        = bc_tick,
    .input       = bc_input,
    .draw        = bc_draw,
};
