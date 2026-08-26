/*
 * gate_app.c -- the application: a Battle Chip Gate on one side, a screen and
 * an encoder on the other.
 *
 * This is what tasks.c was calling into: gba_link_service() and ui_service()
 * were declared and never defined, so the firmware linked against nothing.
 *
 * The gate loop is four lines and deliberately so. Everything hard already
 * lives in tested modules -- chipgate.c owns the protocol, chipview owns the
 * selection, chipdb owns the data. This file only wires them together.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "chipdb.h"
#include "chipgate.h"
#include "link_source.h"
#include "ui_chipview.h"
#include "ui_list.h"
#include "ui_render.h"
#include "ui_shell.h"
#include "variant.h"

static const char *TAG = "gate";

/* ---- embedded databases ------------------------------------------------ */
/*
 * EMBED_FILES puts each .chdb straight in flash and gives us its bounds. No
 * filesystem involved, so a gate works with nothing plugged in -- which
 * matters, because the USB volume is unavailable exactly when a PC has it.
 */
#if defined(GBAGATE_HAS_CHIPDB)
#define DB_SYM(name) \
    extern const uint8_t name##_start[] asm("_binary_" #name "_chdb_start"); \
    extern const uint8_t name##_end[]   asm("_binary_" #name "_chdb_end");

DB_SYM(bn4r) DB_SYM(bn4b) DB_SYM(bn5p) DB_SYM(bn5c) DB_SYM(bn6g) DB_SYM(bn6f)

static const struct {
    cg_game_t      game;
    const char    *label;
    const uint8_t *start, *end;
} k_databases[] = {
    { CG_GAME_BN4,    "bn4r", bn4r_start, bn4r_end },
    { CG_GAME_BN4,    "bn4b", bn4b_start, bn4b_end },
    { CG_GAME_BN5,    "bn5p", bn5p_start, bn5p_end },
    { CG_GAME_BN5,    "bn5c", bn5c_start, bn5c_end },
    { CG_GAME_BN6_US, "bn6g", bn6g_start, bn6g_end },
    { CG_GAME_BN6_US, "bn6f", bn6f_start, bn6f_end },
};
#define DB_COUNT (sizeof k_databases / sizeof k_databases[0])
#endif

/* ---- state ------------------------------------------------------------- */

static chipgate_t  s_gate;
static chipdb_t    s_db;
static chipview_t  s_view;
static ui_list_t   s_list;
static uint16_t    s_view_storage[512];
static bool        s_db_open;

static uint32_t s_transfers;
static uint32_t s_chips_sent;
static char     s_status[32] = "WAITING";

/* ---- database ---------------------------------------------------------- */

bool gate_open_database(const char *label) {
#if defined(GBAGATE_HAS_CHIPDB)
    for (unsigned i = 0; i < DB_COUNT; i++) {
        if (strcmp(k_databases[i].label, label) != 0) {
            continue;
        }
        size_t size = (size_t)(k_databases[i].end - k_databases[i].start);
        if (!chipdb_open(&s_db, k_databases[i].start, size)) {
            ESP_LOGE(TAG, "%s failed to open (%u bytes) -- truncated embed?",
                     label, (unsigned)size);
            return false;
        }
        chipgate_init(&s_gate, k_databases[i].game);
        chipview_init(&s_view, &s_db, s_view_storage,
                      sizeof s_view_storage / sizeof s_view_storage[0]);
        ui_list_init(&s_list, UI_ROWS, true);
        ui_list_set(&s_list, s_view.view, s_view.view_count);
        s_db_open = true;

        ESP_LOGI(TAG, "%s: %u chips, gate id %04X",
                 label, chipdb_count(&s_db), s_gate.gate_id);
        return true;
    }
    ESP_LOGE(TAG, "no embedded database called '%s'", label);
#else
    (void)label;
    ESP_LOGE(TAG, "this image was built without chip databases");
#endif
    return false;
}

/* ---- the gate ---------------------------------------------------------- */

/*
 * One transfer. Called in a tight loop from the link task on core 1.
 *
 * The staging order is the part that matters: the answer must be handed to the
 * source *before* the parent clocks our slot, which is why chipgate_step()
 * runs on the word we just received and stages immediately rather than
 * deferring to the next iteration.
 */
void gba_link_service(void) {
    const link_source_t *src = link_source_active();
    if (!src || !src->poll) {
        vTaskDelay(pdMS_TO_TICKS(100));
        return;
    }

    uint16_t from_game;
    if (!src->poll(&from_game, 50)) {
        return;   /* no console attached; normal */
    }

    uint16_t answer = chipgate_step(&s_gate, from_game);
    src->stage(answer);
    s_transfers++;

    if (chipgate_take_consumed(&s_gate)) {
        s_chips_sent++;
    }
}

/* Called from the UI when the user slots a chip in. */
bool gate_send_chip(uint16_t wire_id) {
    if (!chipgate_load(&s_gate, wire_id)) {
        snprintf(s_status, sizeof s_status, "BUSY");
        return false;
    }
    snprintf(s_status, sizeof s_status, "SENT %04X", wire_id);
    return true;
}

/* ---- the screen -------------------------------------------------------- */

typedef enum {
    UI_EVENT_ENCODER = 0, UI_EVENT_CHIP_IN, UI_EVENT_CHIP_OUT,
    UI_EVENT_LINK_UP, UI_EVENT_LINK_DOWN, UI_EVENT_CHIP_SENT,
    UI_EVENT_USB_ATTACHED, UI_EVENT_USB_DETACHED
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    int32_t         a;
    uint16_t        b;
} ui_event_t;

extern uint16_t *st7735_framebuffer(void);
extern void      st7735_mark_all_dirty(void);
extern size_t    st7735_flush(void);

void ui_service(const ui_event_t *e, bool have_event) {
    if (!s_db_open) {
        return;
    }

    if (have_event && e) {
        switch (e->type) {
        case UI_EVENT_ENCODER:
            ui_list_move(&s_list, e->a);
            break;
        case UI_EVENT_CHIP_SENT: {
            uint16_t pos;
            if (ui_list_selected(&s_list, &pos)) {
                uint16_t slot = chipview_slot_at(&s_view, s_list.cursor);
                /* Slot 5 carries the wire ID -- the number a physical chip's
                 * shorted pads form -- not chip_record_t.index, which is the
                 * table's sort key and names a different chip for most of the
                 * database. tools/check_wire_ids.py quantifies the divergence. */
                uint16_t wire_id = chipdb_id_for_slot(&s_db, slot);
                if (wire_id == CHIPDB_NO_ID) {
                    /* No physical chip carries this record: refuse rather than
                     * put a plausible-looking wrong number on the wire. */
                    snprintf(s_status, sizeof s_status, "NO WIRE ID");
                } else {
                    gate_send_chip(wire_id);
                }
            }
            break;
        }
        case UI_EVENT_LINK_UP:
            snprintf(s_status, sizeof s_status, "LINKED");
            break;
        case UI_EVENT_LINK_DOWN:
            snprintf(s_status, sizeof s_status, "WAITING");
            break;
        default:
            break;
        }
    }

    /* The list must stay backed by this view or the renderer refuses to draw
     * it -- see ui_render_list_matches(). Re-binding here keeps them in step
     * after a tab change. */
    if (s_list.items != s_view.view || s_list.count != s_view.view_count) {
        ui_list_set(&s_list, s_view.view, s_view.view_count);
    }

    if (!s_list.dirty && !have_event) {
        return;
    }
    s_list.dirty = false;

    uint16_t *fb = st7735_framebuffer();
    if (!fb) {
        return;
    }
    ui_canvas_t canvas = { fb, UI_SCREEN_W, UI_SCREEN_H };
    ui_render_chip_screen(&canvas, &s_view, &s_db, &s_list, s_status);
    st7735_mark_all_dirty();
    st7735_flush();
}

/* ---- observability ------------------------------------------------------ */

void gate_stats(uint32_t *transfers, uint32_t *chips, bool *linked) {
    if (transfers) *transfers = s_transfers;
    if (chips)     *chips = s_chips_sent;
    if (linked)    *linked = chipgate_connected(&s_gate);
}

const chipgate_t *gate_state(void) { return &s_gate; }
chipview_t       *gate_view(void)  { return &s_view; }
ui_list_t        *gate_list(void)  { return &s_list; }
