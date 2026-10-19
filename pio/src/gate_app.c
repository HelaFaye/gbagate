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

/* esp_err.h does not exist in ESP-IDF; error types are defined in esp_system.h */
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   /* vTaskDelay */

#include "chipdb.h"
#include "chipgate.h"
#include "link_source.h"
#include "ui_chipview.h"
#include "ui_list.h"
#include "ui_font.h"    /* UI_FONT_H, for centring the banner */
#include "ui_render.h"
#include "controls.h"
#include "ir_cdz.h"
#include "ui_romview.h"
#include "ui_settings.h"
#include "chip_slot.h"
#include "chip_reader.h"
#include "link_bridge.h"
#include "settings.h"
#include "gba_multiboot.h"

/* Supplied by port/esp32s3/mb_spi_esp32s3.c on hardware. QEMU has no SPI, so
 * Send Program is a hardware-only mode and says so rather than pretending. */
#if defined(GBAGATE_QEMU)
static mb_xfer_fn mb_spi_transfer(void) { return NULL; }
#else
/* Weak, because the link image has no multiboot and does not build
 * mb_spi_esp32s3.c; NULL is Send Program's "unavailable" path. */
mb_xfer_fn mb_spi_transfer(void);
__attribute__((weak)) mb_xfer_fn mb_spi_transfer(void) { return NULL; }
#endif
#include "ui_shell.h"
#include "app_modes.h"

extern uint32_t tasks_ui_events_dropped(void);

/*
 * Infrared. Weak defaults so images without a transmitter still link and say
 * so; the real ones would live in port/esp32s3/ir_esp32s3.c, which needs a
 * scope before it is worth writing.
 */
bool     ir_ready(void);
uint32_t ir_commands(void);
uint32_t ir_aborted(void);
uint16_t ir_last_command(void);
bool     ir_send(uint16_t cmd);
__attribute__((weak)) bool     ir_ready(void)        { return false; }
__attribute__((weak)) uint32_t ir_commands(void)     { return 0; }
__attribute__((weak)) uint32_t ir_aborted(void)      { return 0; }
__attribute__((weak)) uint16_t ir_last_command(void) { return 0; }
__attribute__((weak)) bool     ir_send(uint16_t c)   { (void)c; return false; }
extern link_bridge_t *tasks_bridge(void);

#if !defined(GBAGATE_QEMU)
#include "gbl_peer.h"
/* The wireless link cable (port/esp32s3/peer_espnow_esp32s3.c,
 * port/gblink/peer_glue.cpp). */
esp_err_t        wireless_cable_start(uint8_t mode, uint8_t variant);
void             wireless_cable_stop(void);
bool             wireless_cable_peer_mac(uint8_t mac[6]);
const gbl_peer_t *gblink_peer_state(void);
#endif

/*
 * The chip reader, when one is fitted. Weak defaults so the gate and link
 * images -- which have no reader connector -- still link and report honestly
 * that there is nothing to read.
 */
bool chip_reader_detect(bool *advance, bool *beast);
__attribute__((weak)) bool chip_reader_detect(bool *a, bool *b) {
    if (a) *a = false;
    if (b) *b = false;
    return false;
}
/*
 * Same for the bus the reader sits on. gate_scan_chip() names both, and only
 * the full image builds i2c_esp32s3.c, so without these the gate and link
 * hardware images failed to link (found by the first idf.py hardware build).
 * NULL is gate_scan_chip()'s "no reader" path.
 */
#if !defined(GBAGATE_QEMU)
const chip_bus_io_t *chip_reader_bus(void);
void                *chip_reader_ctx(void);
__attribute__((weak)) const chip_bus_io_t *chip_reader_bus(void) { return NULL; }
__attribute__((weak)) void                *chip_reader_ctx(void) { return NULL; }
#endif
static uint16_t gate_scan_chip(bool advance, bool beast);
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
    /* __asm__ rather than asm: the latter is a GNU extension that a strict
     * -std=c11 compiler rejects, which stopped the host lint from ever
     * looking at this file. ESP-IDF's toolchain accepts both. */ \
    extern const uint8_t name##_start[] __asm__("_binary_" #name "_chdb_start"); \
    extern const uint8_t name##_end[]   __asm__("_binary_" #name "_chdb_end");

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

/*
 * The shell was compiled, tested and never drawn: ui_service() went straight
 * to the chip screen, so all seven modes were unreachable and the device
 * booted into a chip list with no way out. This is the dispatcher it was
 * missing.
 */
/*
 * Forward declarations. ui_service() calls into the mode entry points, which
 * are defined below it, and C's implicit-declaration rule turns that into a
 * guessed prototype rather than an error until the real definition disagrees.
 * Nothing on the host compiles this file, so the first thing to notice was the
 * ESP-IDF build with -Werror.
 */
void gate_enter_mode(app_mode_t mode, cg_game_t game);
void gate_apply_display_settings(void);
void gate_shell_start(uint32_t caps);
bool gate_send_chip(uint16_t wire_id);
bool gate_open_database(const char *label);

/* Display glue, defined in port/esp32s3/st7735_*.c. */
extern uint16_t *st7735_framebuffer(void);
extern void      st7735_mark_all_dirty(void);
extern size_t    st7735_flush(void);

#if !defined(GBAGATE_QEMU)
/*
 * Weak default. The real probe lives in port/esp32s3/i2c_esp32s3.c and is only
 * built into the full image, so the gate and link images -- which have no
 * reader connector -- still link, and report "not fitted", which hides Read
 * Chip. That is the honest answer when nothing can read a chip.
 */
__attribute__((weak)) bool chip_reader_probe(void) { return false; }
#endif

/* ---- Send Program ------------------------------------------------------- */

static rom_catalog_t s_cat;
static romview_t     s_roms;
static char          s_send_status[32];
static uint32_t      s_send_pct;

/*
 * Storage. Declared unconditionally so every build type-checks the calls; the
 * weak definitions below cover images that have no storage, and QEMU, which
 * has no USB peripheral at all.
 */
bool            msc_storage_available(void);
bool            msc_rescan(rom_catalog_t *cat, uint32_t logo_reference);
const rom_io_t *msc_io(void);

#if 1   /* always: see the note above */
/*
 * Weak, so port/esp32s3/usb_msc_esp32s3.c replaces them by simply existing.
 * Under emulation nothing does, and Send Program correctly reports that there
 * is no storage rather than offering an empty list as if the drive were blank.
 */
__attribute__((weak)) bool msc_storage_available(void) { return false; }
__attribute__((weak)) bool msc_rescan(rom_catalog_t *c, uint32_t logo) {
    (void)c; (void)logo; return false;
}
__attribute__((weak)) const rom_io_t *msc_io(void) { return NULL; }
#endif

/* ---- persistence -------------------------------------------------------- */

static settings_t s_settings;

/* Real implementations in port/esp32s3/nvs_store.c; weak here so images
 * without persistence still link and simply run on defaults. */
const settings_io_t *nvs_settings_io(void);
__attribute__((weak)) const settings_io_t *nvs_settings_io(void) { return NULL; }

/*
 * Saving is deferred rather than immediate. Every favourite toggle is a flash
 * write otherwise, and flash writes suspend the instruction cache -- the one
 * thing the link task cannot tolerate. Marking dirty and flushing when a mode
 * is left keeps writes to the moments nothing is being clocked.
 */
static bool s_settings_dirty;

static void settings_touch(void) { s_settings_dirty = true; }

static void settings_flush(void) {
    /* Pick up anything the chip screen changed. chipview owns the live copy
     * while a gate session is running; settings owns the stored one. */
    if (s_view.favorites_dirty) {
        memcpy(s_settings.favorites, s_view.favorites,
               sizeof s_settings.favorites < sizeof s_view.favorites
                   ? sizeof s_settings.favorites : sizeof s_view.favorites);
        s_view.favorites_dirty = false;
        s_settings_dirty = true;
    }
    if (!s_settings_dirty) {
        return;
    }
    if (settings_save(&s_settings, nvs_settings_io(), NULL)) {
        s_settings_dirty = false;
    }
}

/* ---- USB storage handover ----------------------------------------------- */

esp_err_t msc_release_to_host(void);
esp_err_t msc_claim_from_host(void);
__attribute__((weak)) esp_err_t msc_release_to_host(void) { return ESP_FAIL; }
__attribute__((weak)) esp_err_t msc_claim_from_host(void) { return ESP_FAIL; }

static bool s_exposed;
static ui_settings_t s_setedit;

/* ---- controls ----------------------------------------------------------- */

/*
 * The input object belongs to whichever driver is running -- buttons_esp32s3.c
 * on hardware, input_qemu.c under emulation. Weak default so an image with
 * neither still links; the screens then bind against nothing and fall back to
 * an empty hint rather than a wrong one.
 */
ui_input_t *input_active(void);
__attribute__((weak)) ui_input_t *input_active(void) { return NULL; }

static controls_t s_controls;
static uint32_t   s_bound_set;   /* what the current screen asked for */

/*
 * Declare a screen's controls. Cheap enough to call every frame, and doing so
 * means a screen cannot forget to rebind after the input mode is changed from
 * the Settings screen -- which would otherwise leave the previous mode's
 * bindings, and a footer describing hardware that is no longer selected.
 */
static const char *screen_controls(uint32_t wanted) {
    ui_input_t *in = input_active();
    if (!in) {
        return "";
    }
    if (wanted != s_bound_set || s_controls.in != in ||
        s_controls.mode != in->mode) {
        controls_bind(&s_controls, in, wanted);
        s_bound_set = wanted;
    }
    return controls_hint(&s_controls);
}

/* What a plain page offers: confirm and back, nothing else. */
#define CTRLS_PAGE   (CTRL_BIT(CTRL_CONFIRM) | CTRL_BIT(CTRL_CANCEL))

/* A list: movement as well. */
#define CTRLS_LIST   (CTRLS_PAGE | CTRL_BIT(CTRL_PREV) | CTRL_BIT(CTRL_NEXT) | \
                      CTRL_BIT(CTRL_PAGE_PREV) | CTRL_BIT(CTRL_PAGE_NEXT))

/* The chip screen: everything. */
#define CTRLS_CHIPS  (CTRLS_LIST | CTRL_BIT(CTRL_TAB_PREV) | \
                      CTRL_BIT(CTRL_TAB_NEXT) | CTRL_BIT(CTRL_MARK) | \
                      CTRL_BIT(CTRL_MENU))

static ui_shell_t s_shell;
static bool       s_shell_ready;

/*
 * The dispatcher.
 *
 * app_modes.c has existed and been tested since 25 Aug with nothing in the
 * firmware calling it: shell_select() set SHELL_SCREEN_RUNNING, ui_service()
 * called gate_enter_mode() by hand, and no mode had a teardown at all. Every
 * mode now enters and leaves through here, so a single exit path runs the
 * claim-back, the settings flush and the chip cancel that used to be scattered
 * through the event handling -- and reached only via the Back key.
 */
static app_dispatch_t s_dispatch;

/*
 * Why the last attempt to start a mode failed. Held separately because
 * app_dispatch_from_shell() resets last_status to OK as soon as the shell
 * leaves the running screen, which is exactly the moment we want to explain.
 */
static app_dispatch_status_t s_last_refusal = APP_DISPATCH_OK;

static uint32_t s_last_transfer_ms;

static uint32_t s_transfers;
static uint32_t s_chips_sent;
static char     s_status[32] = "WAITING";

/*
 * How long a chip may sit undelivered before the gate gives up on it. A
 * frame-paced game completes the 81 transfers a chip needs in about 1.4 s, so
 * three seconds is comfortably past "slow" and well short of "the user has
 * noticed nothing is happening".
 */
#define GATE_STALL_MS 3000

/*
 * Self-test sends a chip on its own. Two of the ROM's six checks -- that a chip
 * arrives in slot 5, and that it retires after exactly nine rotations -- cannot
 * pass unless something sends one, and requiring a human to press a button
 * halfway through an automated run makes it not automated.
 *
 * The value matters. A watcher on the far end identifies slots by position,
 * and position can drift, so the test chip must not be a value either rolling
 * counter can take. DATA0 is 8-bit and eventually takes every value up to
 * 0xFF; DATA1 always has its low ten bits clear. 0x0101 avoids both and is a
 * real chip in every database. Chip 1 does not -- the counter reaching 0x0001
 * reads as a chip arriving, which is how this was found.
 */
#define SELFTEST_CHIP        0x0101u
#define SELFTEST_SETTLE      40      /* transfers to let the link settle   */

/*
 * Transfers to leave empty between chips. The gate retires a chip the instant
 * its ninth repeat goes out, so a firmware that reloads immediately puts the
 * next chip in slot 5 before the far end has seen the slot empty even once --
 * and from out there the two deliveries are one impossibly long one.
 *
 * Slot 5 comes round every nine transfers, so two full rotations is enough to
 * guarantee at least one observed gap, with margin for a dropped frame.
 */
#define SELFTEST_COOLDOWN    20

static bool     s_selftest;
static uint32_t s_selftest_sent;
static uint32_t s_selftest_ready;

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
        /*
         * Restore favourites. settings.c has carried this array since it was
         * written and nothing ever read it back, so starring a chip survived
         * exactly until the next power cycle -- and looked like the save was
         * broken rather than never loaded.
         */
        memcpy(s_view.favorites, s_settings.favorites,
               sizeof s_view.favorites < sizeof s_settings.favorites
                   ? sizeof s_view.favorites : sizeof s_settings.favorites);
        chipview_rebuild(&s_view);
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
        /*
         * No transfer. A chip only leaves the gate when the rotation reaches
         * its slot, which takes 81 transfers -- so if the parent stops
         * clocking mid-chip, chip_id never clears and chipgate_load() refuses
         * every future chip with BUSY, for ever.
         *
         * That is not hypothetical: it is what happens when the tester ROM
         * finishes and stops driving, and it is what happens on real hardware
         * when someone unplugs the cable. Give up on the chip rather than
         * wedging the interface.
         */
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (chipgate_busy(&s_gate) && s_last_transfer_ms &&
            (now - s_last_transfer_ms) > GATE_STALL_MS) {
            chipgate_clear(&s_gate);
            snprintf(s_status, sizeof s_status, "LINK STALLED");
            s_last_transfer_ms = 0;
        }
        return;
    }

    s_last_transfer_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /*
     * Two chips, spaced far enough apart that the first has retired. The
     * second is what proves the slot really freed rather than the first
     * simply never having been counted.
     */
    if (s_selftest && chipgate_connected(&s_gate) && !chipgate_busy(&s_gate)) {
        if (s_selftest_ready == 0) {
            s_selftest_ready = s_transfers + SELFTEST_SETTLE;
        }
        if (s_selftest_sent < 2 && s_transfers >= s_selftest_ready) {
            if (chipgate_load(&s_gate, SELFTEST_CHIP)) {
                s_selftest_sent++;
                s_selftest_ready = 0;   /* re-armed by the cooldown below */
                snprintf(s_status, sizeof s_status, "TEST CHIP %u",
                         (unsigned)s_selftest_sent);
            }
        }
    }

    uint16_t answer = chipgate_step(&s_gate, from_game);
    src->stage(answer);
    s_transfers++;

    if (chipgate_take_consumed(&s_gate)) {
        s_chips_sent++;
        /* Leave the slot visibly empty before reloading. See SELFTEST_COOLDOWN. */
        s_selftest_ready = s_transfers + SELFTEST_COOLDOWN;
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

/*
 * A running mode that has no screen of its own used to render nothing at all,
 * which looks exactly like a freeze: the panel keeps showing whatever was on it
 * when the mode was entered, and only Back gets out. Every mode now draws
 * something, even if that something is just its own name.
 */
static void render_mode_screen(void)
{
    uint16_t *fb = st7735_framebuffer();
    if (!fb) {
        return;
    }
    ui_canvas_t canvas = { fb, UI_SCREEN_W, UI_SCREEN_H };

    ui_fill(&canvas, 0, 0, UI_SCREEN_W, UI_SCREEN_H, COL_BG);
    ui_fill(&canvas, 0, 0, UI_SCREEN_W, UI_HEADER_H, COL_HEADER);
    ui_text_clipped(&canvas, 3, (UI_HEADER_H - UI_FONT_H) / 2, UI_SCREEN_W - 6,
                    shell_mode_name(s_shell.chosen_mode), COL_BG);

    int y = UI_LIST_Y + 2;
    char line[32];

    if (s_shell.chosen_mode == APP_MODE_DIAGNOSTICS) {
        /*
         * Diagnostics is the mode you reach for when nothing else works, so it
         * reports the things that are otherwise only visible in a serial log:
         * which capabilities were detected, whether the link ever moved, and
         * whether the UI is losing events.
         */
        char caps[64];
        hw_caps_describe(s_shell.caps, caps, sizeof caps);
        ui_text_clipped(&canvas, 2, y, UI_SCREEN_W - 4, "detected:", COL_TEXT_DIM);
        y += 10;
        ui_text_clipped(&canvas, 2, y, UI_SCREEN_W - 4, caps, COL_TEXT);
        y += 14;

        snprintf(line, sizeof line, "link   %s",
                 chipgate_connected(&s_gate) ? "linked" : "waiting");
        ui_text_clipped(&canvas, 2, y, UI_SCREEN_W - 4, line, COL_TEXT);
        y += 10;

        snprintf(line, sizeof line, "xfers  %lu", (unsigned long)s_transfers);
        ui_text_clipped(&canvas, 2, y, UI_SCREEN_W - 4, line, COL_TEXT);
        y += 10;

        snprintf(line, sizeof line, "chips  %lu", (unsigned long)s_chips_sent);
        ui_text_clipped(&canvas, 2, y, UI_SCREEN_W - 4, line, COL_TEXT);
        y += 10;

        snprintf(line, sizeof line, "gate   %04X", s_gate.gate_id);
        ui_text_clipped(&canvas, 2, y, UI_SCREEN_W - 4, line, COL_TEXT);
        y += 10;

        snprintf(line, sizeof line, "uidrop %lu",
                 (unsigned long)tasks_ui_events_dropped());
        ui_text_clipped(&canvas, 2, y, UI_SCREEN_W - 4, line, COL_TEXT);
    } else if (!app_dispatch_running(&s_dispatch)) {
        /*
         * The shell is on the running screen with nothing behind it, which is
         * what a refused enter() looks like. Naming the reason matters: the
         * page used to read "no screen for this mode yet", which describes the
         * firmware rather than the fault the user just hit.
         */
        ui_text_clipped(&canvas, 2, y, UI_SCREEN_W - 4,
                        "mode did not start", COL_TEXT);
        y += 14;
        ui_text_clipped(&canvas, 2, y, UI_SCREEN_W - 4,
                        app_dispatch_status_name(s_dispatch.last_status),
                        COL_TEXT_DIM);
    } else {
        ui_text_clipped(&canvas, 2, y, UI_SCREEN_W - 4, s_status, COL_TEXT);
        y += 14;
        ui_text_clipped(&canvas, 2, y, UI_SCREEN_W - 4,
                        "no screen for this", COL_TEXT_DIM);
        y += 10;
        ui_text_clipped(&canvas, 2, y, UI_SCREEN_W - 4,
                        "mode yet", COL_TEXT_DIM);
    }

    int fy = UI_SCREEN_H - UI_FOOTER_H;
    ui_fill(&canvas, 0, fy, UI_SCREEN_W, UI_FOOTER_H, COL_ROW_ALT);
    ui_fill(&canvas, 0, fy, UI_SCREEN_W, 1, COL_HEADER);
    ui_text_clipped(&canvas, 2, fy + (UI_FOOTER_H - UI_FONT_H) / 2,
                    UI_SCREEN_W - 4, screen_controls(CTRLS_PAGE), COL_TEXT_DIM);

    st7735_mark_all_dirty();
    st7735_flush();
}

static void send_progress(void *ctx, uint32_t sent, uint32_t total)
{
    (void)ctx;
    s_send_pct = total ? (sent * 100u) / total : 0;
}

/*
 * Send the selected program.
 *
 * Staging first, then transferring, with the file handle closed in between --
 * see the staging rule in rom_catalog.h. The whole thing blocks the UI task
 * for the duration, which is deliberate: the user pressed send, a progress
 * screen is the expected response, and interleaving it with menu input would
 * mean handling a cancel mid-transfer that the protocol has no way to express.
 */
static void send_program(void)
{
    const rom_entry_t *e = romview_current(&s_roms);
    if (!e || !romview_selectable(&s_roms, s_roms.cursor)) {
        return;
    }

    /*
     * Checked before the 256 KB staging buffer is taken, not after. With no SPI
     * master in this image -- QEMU -- mb_send() answers MB_ERR_ARGS, and "bad
     * arguments" describes the call rather than the cause: nothing here can
     * drive the clock.
     */
    mb_xfer_fn xfer_fn = mb_spi_transfer();
    if (!xfer_fn) {
        snprintf(s_send_status, sizeof s_send_status, "no link hardware");
        return;
    }

    /*
     * 256 KB, allocated for the transfer and released after.
     *
     * As a static array this sat in .bss for the life of the device and
     * overflowed DRAM by ~3 KB: the S3 has about 320 KB total and ESP-IDF,
     * FreeRTOS, TinyUSB and the framebuffer want most of it. Reserving the
     * largest buffer in the project permanently, for a mode used occasionally,
     * was the wrong trade -- and it failed at link time rather than at runtime,
     * which is the good outcome.
     *
     * PSRAM first: the transfer is SPI-master paced, so a slower buffer only
     * stretches clocks the GBA is waiting on anyway. Internal DRAM is the
     * fallback for boards without PSRAM, and the failure is reported rather
     * than crashed on.
     */
    uint8_t *staging = heap_caps_malloc(MB_MAX_SIZE, MALLOC_CAP_SPIRAM);
    if (!staging) {
        staging = heap_caps_malloc(MB_MAX_SIZE, MALLOC_CAP_8BIT);
    }
    if (!staging) {
        snprintf(s_send_status, sizeof s_send_status, "out of memory");
        return;
    }

    uint32_t len = 0;

    snprintf(s_send_status, sizeof s_send_status, "staging");
    s_send_pct = 0;

    const rom_io_t *io = msc_io();
    if (!io || !rom_stage(&s_cat, s_roms.cursor, io, NULL,
                          staging, MB_MAX_SIZE, &len)) {
        snprintf(s_send_status, sizeof s_send_status, "stage failed");
        heap_caps_free(staging);
        return;
    }

    snprintf(s_send_status, sizeof s_send_status, "power on the GBA");
    s_send_pct = 0;

    mb_session_t mb;
    mb_init(&mb, xfer_fn, NULL);
    mb.progress = send_progress;

    mb_result_t r = mb_send(&mb, staging, len);
    if (r == MB_OK) {
        snprintf(s_send_status, sizeof s_send_status, "sent");
        s_send_pct = 100;
    } else {
        snprintf(s_send_status, sizeof s_send_status, "%s", mb_result_name(r));
    }

    heap_caps_free(staging);
}

/*
 * A titled page of lines. The link modes, Read Chip and USB Storage are all
 * "show a few facts and one instruction", and giving each its own renderer
 * would be four copies of the same layout drifting apart.
 */
static void render_page(const char *title, const char *const *lines, int n,
                        const char *footer)
{
    uint16_t *fb = st7735_framebuffer();
    if (!fb) {
        return;
    }
    ui_canvas_t canvas = { fb, UI_SCREEN_W, UI_SCREEN_H };

    ui_fill(&canvas, 0, 0, UI_SCREEN_W, UI_SCREEN_H, COL_BG);
    ui_fill(&canvas, 0, 0, UI_SCREEN_W, UI_HEADER_H, COL_HEADER);
    ui_text_clipped(&canvas, 3, (UI_HEADER_H - UI_FONT_H) / 2, UI_SCREEN_W - 6,
                    title, COL_BG);

    for (int i = 0; i < n && i < 8; i++) {
        if (!lines[i]) {
            continue;
        }
        ui_text_clipped(&canvas, 2, UI_LIST_Y + 4 + i * 11, UI_SCREEN_W - 4,
                        lines[i], i == 0 ? COL_TEXT : COL_TEXT_DIM);
    }

    int fy = UI_SCREEN_H - UI_FOOTER_H;
    ui_fill(&canvas, 0, fy, UI_SCREEN_W, UI_FOOTER_H, COL_ROW_ALT);
    ui_fill(&canvas, 0, fy, UI_SCREEN_W, 1, COL_HEADER);
    ui_text_clipped(&canvas, 2, fy + (UI_FOOTER_H - UI_FONT_H) / 2,
                    UI_SCREEN_W - 4, footer ? footer : "", COL_TEXT_DIM);

    st7735_mark_all_dirty();
    st7735_flush();
}

/*
 * The four modes whose whole interface is a page of facts and one button.
 * Written together because they share the shape; splitting them would be four
 * copies of the same event handling.
 */
/*
 * Read whichever slot has a chip in it and decode a wire ID.
 *
 * The bus and the reader are both already tested; this is only the join. A
 * failed scan returns CG_CHIP_NONE rather than a plausible ID, because a wrong
 * ID sends the wrong chip and looks like a database fault.
 */
static uint16_t gate_scan_chip(bool advance, bool beast)
{
#if defined(GBAGATE_QEMU)
    (void)advance; (void)beast;
    return CG_CHIP_NONE;   /* no I2C under emulation */
#else
    extern const chip_bus_io_t *chip_reader_bus(void);
    extern void               *chip_reader_ctx(void);

    const chip_bus_io_t *bus = chip_reader_bus();
    if (!bus) {
        return CG_CHIP_NONE;
    }

    chip_slots_t slots;
    chip_slots_init(&slots, bus, chip_reader_ctx());

    chip_family_t fam;
    uint16_t pads;
    if (chip_slots_scan(&slots, advance, beast, &fam, &pads) != SLOT_SCAN_OK) {
        return CG_CHIP_NONE;
    }

    chip_reader_t rd;
    chip_reader_init(&rd, fam, NULL, NULL);
    return chip_reader_decode(&rd, pads);
#endif
}

static void render_settings(const ui_event_t *e, bool have_event)
{
    if (have_event && e) {
        if (e->type == UI_EVENT_ENCODER) {
            uiset_move(&s_setedit, e->a);
        } else if (e->type == UI_EVENT_CHIP_SENT) {
            uiset_toggle_edit(&s_setedit);
        }
    }

    if (uiset_dirty(&s_setedit)) {
        settings_touch();
        uiset_clear_dirty(&s_setedit);
        /* Apply immediately: an offset you cannot see change is one you cannot
         * calibrate by eye, which is the only way it gets calibrated. */
        if (uiset_display_affected(&s_setedit)) {
            gate_apply_display_settings();
        }
    }

    uint16_t *fb = st7735_framebuffer();
    if (!fb) {
        return;
    }
    ui_canvas_t canvas = { fb, UI_SCREEN_W, UI_SCREEN_H };
    ui_render_menu(&canvas, "Settings", uiset_label, &s_setedit,
                   uiset_count(), s_setedit.cursor,
                   /* The editor's own hint says whether turning changes the
                    * value or the selection, which no binding table knows. */
                   uiset_footer(&s_setedit));
    st7735_mark_all_dirty();
    st7735_flush();
}

static void render_simple_mode(const ui_event_t *e, bool have_event)
{
    char l0[32], l1[32], l2[32], l3[32];
    const char *lines[4] = { l0, l1, l2, l3 };
    const char *footer = NULL;
    const char *title = shell_mode_name(s_shell.chosen_mode);

    l0[0] = l1[0] = l2[0] = l3[0] = '\0';

    switch (s_shell.chosen_mode) {

#if !defined(GBAGATE_QEMU)
    case APP_MODE_LINK_WIRELESS: {
        /* The wireless link cable: GB-Link's relay, run against another board
         * over ESP-NOW (src/gbl_peer.c). Retransmits climbing while frames
         * still flow is a noisy channel; frames stopping is the peer gone. */
        const gbl_peer_t *p = gblink_peer_state();
        uint8_t mac[6];
        snprintf(l0, sizeof l0, "%s%s", gbl_peer_state_name(p->state),
                 p->state >= GBL_PEER_SETTLING ? (p->master ? " (M)" : " (S)") : "");
        if (wireless_cable_peer_mac(mac)) {
            snprintf(l1, sizeof l1, "%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            snprintf(l1, sizeof l1, "no peer yet");
        }
        snprintf(l2, sizeof l2, "out %lu in %lu", (unsigned long)p->data_out, (unsigned long)p->data_in);
        snprintf(l3, sizeof l3, "resent %lu", (unsigned long)p->retransmits);
        footer = screen_controls(CTRLS_PAGE);
        break;
    }
#endif

    case APP_MODE_LINK_USB:
#if defined(GBAGATE_QEMU)
    case APP_MODE_LINK_WIRELESS:   /* QEMU has no radio: the UART bridge */
#endif
    {
        link_bridge_t *b = tasks_bridge();
        snprintf(l0, sizeof l0, "%s", b ? bridge_state_name(b->state) : "no bridge");
        snprintf(l1, sizeof l1, "sent   %lu", b ? (unsigned long)b->words_sent : 0ul);
        snprintf(l2, sizeof l2, "recvd  %lu", b ? (unsigned long)b->words_received : 0ul);
        /* Round trip is the number that says whether a wireless link is
         * usable: under a millisecond is fine, tens are not. */
        snprintf(l3, sizeof l3, "rtt    %lu us", b ? (unsigned long)b->rtt_us : 0ul);
        footer = screen_controls(CTRLS_PAGE);
        break;
    }

    case APP_MODE_INFRARED: {
        uint16_t last = ir_last_command();
        snprintf(l0, sizeof l0, "%s", ir_ready() ? "IR ready" : "no IR fitted");
        snprintf(l1, sizeof l1, "cmds   %lu", (unsigned long)ir_commands());
        snprintf(l2, sizeof l2, "last   %03X  %X/%X/%X", (unsigned)last,
                 cdz_category(last), cdz_motion(last), cdz_action(last));
        /*
         * Aborted trains is the number that tells you whether the timing is
         * right. A decoder measuring the wrong thing aborts nearly everything
         * while still looking alive, so a bare command count would not show it.
         */
        snprintf(l3, sizeof l3, "aborted %lu", (unsigned long)ir_aborted());
        footer = screen_controls(CTRLS_PAGE);
        if (have_event && e && e->type == UI_EVENT_CHIP_SENT && ir_ready()) {
            ir_send(last);
        }
        break;
    }

    case APP_MODE_CHIP_SCAN: {
        bool adv = false, beast = false;
        bool ok = chip_reader_detect(&adv, &beast);
        snprintf(l0, sizeof l0, ok ? "reader ready" : "no reader");
        snprintf(l1, sizeof l1, "advance %s", adv ? "chip in" : "empty");
        snprintf(l2, sizeof l2, "beast   %s", beast ? "chip in" : "empty");
        if (ok && (adv || beast)) {
            uint16_t id = gate_scan_chip(adv, beast);
            if (id != CG_CHIP_NONE) {
                snprintf(l3, sizeof l3, "id     %04X", id);
            } else {
                snprintf(l3, sizeof l3, "unreadable");
            }
        }
        footer = screen_controls(CTRLS_PAGE);
        if (have_event && e && e->type == UI_EVENT_CHIP_SENT && (adv || beast)) {
            uint16_t id = gate_scan_chip(adv, beast);
            if (id != CG_CHIP_NONE) {
                gate_send_chip(id);
            }
        }
        break;
    }

    case APP_MODE_USB_STORAGE:
        if (have_event && e && e->type == UI_EVENT_CHIP_SENT) {
            if (s_exposed) {
                s_exposed = msc_claim_from_host() != ESP_OK;
            } else {
                s_exposed = msc_release_to_host() == ESP_OK;
            }
        }
        snprintf(l0, sizeof l0, s_exposed ? "drive is the PC's"
                                          : "drive is ours");
        snprintf(l1, sizeof l1, s_exposed ? "safe to copy files"
                                          : "press to hand over");
        /* One owner at a time, and saying which is the whole point of the
         * mode -- guessing at it is what corrupts a FAT volume. */
        snprintf(l2, sizeof l2, "%s", s_exposed ? "eject before back"
                                                : "");
        /*
         * The hint names the control; the line above already says what the
         * action does, and which way round it is. Restating "enter" here was
         * wrong on every board without a keyboard.
         */
        footer = screen_controls(CTRLS_PAGE);
        break;

    case APP_MODE_SETTINGS:
    default:
        /* Handled by its own renderer: it is a list, not a page of facts. */
        break;
    }

    render_page(title, lines, 4, footer);
}

static void render_rom_browser(void)
{
    uint16_t *fb = st7735_framebuffer();
    if (!fb) {
        return;
    }
    ui_canvas_t canvas = { fb, UI_SCREEN_W, UI_SCREEN_H };

    /* Mid-transfer the list is meaningless; show progress instead. */
    if (s_send_status[0]) {
        ui_fill(&canvas, 0, 0, UI_SCREEN_W, UI_SCREEN_H, COL_BG);
        ui_fill(&canvas, 0, 0, UI_SCREEN_W, UI_HEADER_H, COL_HEADER);
        ui_text_clipped(&canvas, 3, (UI_HEADER_H - UI_FONT_H) / 2,
                        UI_SCREEN_W - 6, "Send Program", COL_BG);

        const rom_entry_t *e = romview_current(&s_roms);
        ui_text_clipped(&canvas, 2, UI_LIST_Y + 4, UI_SCREEN_W - 4,
                        e ? e->name : "", COL_TEXT);
        ui_text_clipped(&canvas, 2, UI_LIST_Y + 22, UI_SCREEN_W - 4,
                        s_send_status, COL_CURSOR);

        /* A bar, because a percentage alone does not show that a transfer has
         * stopped moving. */
        int bw = UI_SCREEN_W - 8;
        ui_fill(&canvas, 4, UI_LIST_Y + 40, bw, 8, COL_BAR_TRACK);
        ui_fill(&canvas, 4, UI_LIST_Y + 40,
                (int)((bw * (int)s_send_pct) / 100), 8, COL_BAR_THUMB);

        char pct[12];   /* "100%" plus slack: %lu can print ten digits */
        snprintf(pct, sizeof pct, "%lu%%", (unsigned long)s_send_pct);
        ui_text_clipped(&canvas, 4, UI_LIST_Y + 54, 60, pct, COL_TEXT_DIM);

        int fy = UI_SCREEN_H - UI_FOOTER_H;
        ui_fill(&canvas, 0, fy, UI_SCREEN_W, UI_FOOTER_H, COL_ROW_ALT);
        ui_fill(&canvas, 0, fy, UI_SCREEN_W, 1, COL_HEADER);
        ui_text_clipped(&canvas, 2, fy + (UI_FOOTER_H - UI_FONT_H) / 2,
                        UI_SCREEN_W - 4, screen_controls(CTRLS_PAGE), COL_TEXT_DIM);
        st7735_mark_all_dirty();
        st7735_flush();
        return;
    }

    if (romview_count(&s_roms) == 0) {
        ui_fill(&canvas, 0, 0, UI_SCREEN_W, UI_SCREEN_H, COL_BG);
        ui_fill(&canvas, 0, 0, UI_SCREEN_W, UI_HEADER_H, COL_HEADER);
        ui_text_clipped(&canvas, 3, (UI_HEADER_H - UI_FONT_H) / 2,
                        UI_SCREEN_W - 6, "Send Program", COL_BG);
        ui_text_clipped(&canvas, 2, UI_LIST_Y + 20, UI_SCREEN_W - 4,
                        romview_empty_text(&s_roms), COL_TEXT);
        st7735_mark_all_dirty();
        st7735_flush();
        return;
    }

    ui_render_menu(&canvas, "Send Program", romview_label, &s_roms,
                   romview_count(&s_roms), (uint8_t)s_roms.cursor,
                   /* romview's hint explains why a rom cannot be sent, which
                    * is about the file rather than the buttons. */
                   romview_hint(&s_roms));
    st7735_mark_all_dirty();
    st7735_flush();
}

/* Labels for ui_render_menu(), which is independent of ui_shell. */
static const char *shell_label(void *ctx, uint8_t index) {
    (void)ctx;
    return shell_label_at(&s_shell, index);
}

static void render_shell(void)
{
    uint16_t *fb = st7735_framebuffer();
    if (!fb) {
        /* Once, not every frame: 60 identical lines a second buries the log
         * that would otherwise explain the black panel. */
        static bool moaned;
        if (!moaned) {
            moaned = true;
            ESP_LOGE(TAG, "no framebuffer -- st7735_init() did not succeed, "
                          "so nothing can be drawn");
        }
        return;
    }
    ui_canvas_t canvas = { fb, UI_SCREEN_W, UI_SCREEN_H };

    const char *title = (s_shell.screen == SHELL_SCREEN_GAMES)
                            ? "Choose game" : "gbagate";

    /*
     * A mode that refused to start says so here. Without it, being bounced
     * straight back to the menu is indistinguishable from a button press that
     * did nothing -- and "the gate never appears" with no explanation is the
     * bug report nobody can act on.
     */
    const char *foot;
    if (s_last_refusal != APP_DISPATCH_OK) {
        foot = app_dispatch_status_name(s_last_refusal);
    } else if (s_shell.screen == SHELL_SCREEN_GAMES) {
        foot = "back: hold";
    } else {
        foot = GBAGATE_VARIANT_NAME;
    }

    (void)screen_controls(CTRLS_LIST);
    ui_render_menu(&canvas, title, shell_label, NULL,
                   shell_item_count(&s_shell), shell_cursor(&s_shell), foot);
    st7735_mark_all_dirty();
    st7735_flush();
}

/*
 * Leave the running mode.
 *
 * The dispatcher follows the shell rather than requiring the caller to pair an
 * enter with an exit, so moving the screen off SHELL_SCREEN_RUNNING is what
 * tears the mode down. That is the whole reason the exit handlers exist: the
 * claim-back, the settings flush and the chip cancel used to be written out at
 * each Back keypress, and every path that was not a Back keypress skipped them.
 */
static void leave_mode(void)
{
    shell_back(&s_shell);
    app_dispatch_from_shell(&s_dispatch, &s_shell);
    render_shell();
}

void ui_service(const ui_event_t *e, bool have_event) {
    if (!s_shell_ready) {
        /*
         * Nothing has called gate_shell_start(), so there is no interface to
         * draw. Returning here leaves the panel showing whatever the
         * framebuffer was initialised to -- black -- and a black screen is
         * indistinguishable from a dead display, a dead UART, a dead task or a
         * wrong QEMU serial mapping. Say which it is instead.
         */
        uint16_t *fb = st7735_framebuffer();
        if (!fb) {
            return;
        }
        ui_canvas_t canvas = { fb, UI_SCREEN_W, UI_SCREEN_H };
        ui_fill(&canvas, 0, 0, UI_SCREEN_W, UI_SCREEN_H, COL_BG);
        ui_fill(&canvas, 0, 0, UI_SCREEN_W, UI_HEADER_H, COL_HEADER);
        ui_text(&canvas, 3, (UI_HEADER_H - UI_FONT_H) / 2, "NO INTERFACE", COL_BG);
        ui_text(&canvas, 4, UI_LIST_Y + 6,  "shell not started", COL_TEXT);
        ui_text(&canvas, 4, UI_LIST_Y + 20, "app_main must call", COL_TEXT_DIM);
        ui_text(&canvas, 4, UI_LIST_Y + 32, "gate_shell_start()", COL_TEXT_DIM);
        ui_text(&canvas, 4, UI_LIST_Y + 52, "display+uart are OK", COL_TEXT_DIM);
        ui_text(&canvas, 4, UI_LIST_Y + 64, "if you can read this", COL_TEXT_DIM);
        st7735_mark_all_dirty();
        st7735_flush();
        return;
    }

    /* ---- the shell, while a mode has not been chosen ---- */
    if (s_shell.screen != SHELL_SCREEN_RUNNING) {
        bool changed = !have_event;   /* first call paints */
        if (have_event && e) {
            switch (e->type) {
            case UI_EVENT_ENCODER:
                shell_move(&s_shell, e->a);
                s_last_refusal = APP_DISPATCH_OK;  /* stale reason, clear it */
                changed = true;
                break;
            case UI_EVENT_CHIP_SENT:      /* the encoder button */
                if (shell_select(&s_shell)) {
                    /*
                     * The join that was missing. The shell is the source of
                     * truth and dispatch follows it, so the game is taken from
                     * the shell rather than passed alongside -- has_game is
                     * authoritative and the cg_game_t beside it means nothing
                     * without it, which is the stale-identity guard
                     * app_modes.h describes.
                     */
                    app_dispatch_status_t st =
                        app_dispatch_from_shell(&s_dispatch, &s_shell);
                    if (st != APP_DISPATCH_OK) {
                        /*
                         * Refused. Do not sit on a running screen with nothing
                         * behind it: go back to where it was chosen and let
                         * the footer say why.
                         */
                        s_last_refusal = st;
                        shell_back(&s_shell);
                    } else {
                        s_last_refusal = APP_DISPATCH_OK;
                    }
                }
                changed = true;
                break;
            case UI_EVENT_CHIP_OUT:       /* back */
                shell_back(&s_shell);
                changed = true;
                break;
            default:
                break;
            }
        }
        if (changed && s_shell.screen != SHELL_SCREEN_RUNNING) {
            render_shell();
        } else if (s_shell.screen == SHELL_SCREEN_RUNNING) {
            ui_service(NULL, false);   /* paint the mode we just entered */
        }
        return;
    }

    /* ---- a mode is running ---- */

    /*
     * Service it before drawing. Only the link modes use this today -- it is
     * what finally calls bridge_ping(), so the round-trip time on their screen
     * is a measurement rather than the zero it has always shown.
     */
    app_dispatch_tick(&s_dispatch);

    /* ---- the modes that are a page plus one action ---- */
    if (s_shell.chosen_mode == APP_MODE_LINK_USB ||
        s_shell.chosen_mode == APP_MODE_LINK_WIRELESS ||
        s_shell.chosen_mode == APP_MODE_CHIP_SCAN ||
        s_shell.chosen_mode == APP_MODE_USB_STORAGE ||
        s_shell.chosen_mode == APP_MODE_SETTINGS) {

        /* The claim-back and the settings flush that used to be written out
         * here are now this mode's exit handler, so they also run when the
         * mode is left any other way. */
        if (have_event && e && e->type == UI_EVENT_CHIP_OUT) {
            leave_mode();
            return;
        }
        if (s_shell.chosen_mode == APP_MODE_SETTINGS) {
            render_settings(e, have_event);
        } else {
            render_simple_mode(e, have_event);
        }
        return;
    }

    if (s_shell.chosen_mode == APP_MODE_MULTIBOOT) {
        if (have_event && e) {
            switch (e->type) {
            case UI_EVENT_ENCODER:
                romview_move(&s_roms, e->a);
                break;
            case UI_EVENT_CHIP_SENT:
                send_program();
                break;
            case UI_EVENT_CHIP_OUT:
                leave_mode();
                return;
            default:
                break;
            }
        }
        render_rom_browser();
        return;
    }

    if (s_shell.chosen_mode != APP_MODE_CHIP_GATE || !s_db_open ||
        !app_dispatch_running(&s_dispatch)) {
        if (have_event && e && e->type == UI_EVENT_CHIP_OUT) {
            leave_mode();
            return;
        }
        /* Redraw every call: the diagnostics counters change on their own, and
         * a screen that only updates on input is indistinguishable from one
         * that has stopped. */
        render_mode_screen();
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
                chip_record_t rec;
                if (chipdb_get(&s_db, slot, &rec)) {
                    gate_send_chip(rec.index);
                }
            }
            break;
        }
        case UI_EVENT_CHIP_OUT:
            /*
             * Back cancels a chip that is still going out, then leaves the
             * mode. Without the cancel, a chip staged against a link that
             * never completes leaves the gate BUSY with no way for the user
             * to clear it.
             */
            if (chipgate_busy(&s_gate)) {
                chipgate_clear(&s_gate);
                snprintf(s_status, sizeof s_status, "CANCELLED");
            } else {
                /* The favourites flush is the mode's exit handler now. */
                leave_mode();
                return;
            }
            break;
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
     * it -- see ui_render_list_matches(). */
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
    /* Bind the full set: the chip screen is the only one that uses tabs, the
     * favourite mark and the sort menu, and binding is what tells ui_input
     * whether A needs a hold on this hardware. */
    (void)screen_controls(CTRLS_CHIPS);
    ui_render_chip_screen(&canvas, &s_view, &s_db, &s_list, s_status);
    st7735_mark_all_dirty();
    st7735_flush();
}

/* Called when the shell hands control to a mode. */
void gate_enter_mode(app_mode_t mode, cg_game_t game) {
    switch (mode) {
    case APP_MODE_CHIP_GATE:
        s_selftest = false;
        gate_open_database(game == CG_GAME_BN4 ? "bn4r" :
                           game == CG_GAME_BN5 ? "bn5p" :
                           game == CG_GAME_BN6_JP ? "bn6g" : "bn6g");
        chipgate_init(&s_gate, game);
        snprintf(s_status, sizeof s_status, "WAITING");
        break;
    case APP_MODE_MULTIBOOT: {
        /*
         * Rescan on entry rather than caching: the drive changes underneath us
         * every time a PC writes to it, and a stale list offers files that are
         * no longer there.
         */
        romview_state_t st = ROMVIEW_NO_STORAGE;
        if (!msc_storage_available()) {
            st = ROMVIEW_HOST_OWNS;
        } else if (msc_rescan(&s_cat, 0)) {
            st = ROMVIEW_OK;
        }
        romview_init(&s_roms);
        romview_set(&s_roms, &s_cat, st);
        s_send_status[0] = '\0';
        s_send_pct = 0;
        break;
    }

    case APP_MODE_SELFTEST:
        /* The gate answers exactly as in normal operation; the tester ROM on
         * the other end does the checking. The only difference is that this
         * mode sends its own chips, so the run needs no one at the keyboard. */
        chipgate_init(&s_gate, game);
        s_selftest       = true;
        s_selftest_sent  = 0;
        s_selftest_ready = 0;
        s_transfers     = 0;
        snprintf(s_status, sizeof s_status, "SELF TEST");
        break;
    default:
        snprintf(s_status, sizeof s_status, "%s", shell_mode_name(mode));
        break;
    }
}

/*
 * Push the stored calibration into the panel.
 *
 * The offsets were being loaded and then ignored: st7735_set_offset() existed
 * and nothing called it, so a device calibrated once came back at defaults and
 * the setting looked like it had not saved. Applying it is what makes the
 * whole persistence path mean anything.
 */
void gate_apply_display_settings(void) {
#if !defined(GBAGATE_QEMU)
    extern void st7735_set_offset(uint8_t colstart, uint8_t rowstart);
    extern void st7735_set_bgr(bool bgr);
    st7735_set_offset(s_settings.colstart, s_settings.rowstart);
    st7735_set_bgr(s_settings.bgr);
#endif
}

void gate_settings_start(void) {
    if (settings_load(&s_settings, nvs_settings_io(), NULL)) {
        ESP_LOGI(TAG, "settings loaded: offsets %u,%u %s",
                 s_settings.colstart, s_settings.rowstart,
                 s_settings.bgr ? "BGR" : "RGB");
#if !defined(GBAGATE_QEMU)
        gate_apply_display_settings();
#endif
    } else {
        /* Not an error. A new device has nothing stored, and the defaults are
         * the ones most 128x128 modules want. */
        ESP_LOGI(TAG, "no stored settings; using defaults");
    }
    s_settings_dirty = false;
    uiset_init(&s_setedit, &s_settings);
}

int gate_input_mode(void) { return (int)s_settings.input_mode; }

const settings_t *gate_settings(void) { return &s_settings; }

/* ---- mode handler table ------------------------------------------------- */
/*
 * The seam app_modes.c was written for and nothing ever bound. Until now
 * s_dispatch was zero-initialised -- no handlers, count 0 -- so every
 * app_dispatch_from_shell() returned NO_HANDLER, gate_enter_mode() was never
 * reached, and picking any mode bounced straight back to the menu with the
 * s_last_refusal footer reading "no handler". PROJECT_STATUS.md calls this out
 * as "designed but not built".
 *
 * Each entry maps a menu mode to the setup, teardown and per-iteration work its
 * subsystem needs. enter() runs when the shell reaches SHELL_SCREEN_RUNNING;
 * exit() runs when it leaves by any route -- Back, a swap, a reset -- so the
 * USB reclaim, the settings flush and the chip cancel happen on every exit path
 * rather than only on the Back key.
 */

static bool mode_enter_chip_gate(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx; (void)has_game;
    gate_enter_mode(APP_MODE_CHIP_GATE, game);
    /* A gate with no database has no identity to announce. Refuse rather than
     * strand the user on a blank chip screen the encoder cannot escape. */
    return s_db_open;
}

static void mode_exit_chip_gate(void *ctx) {
    (void)ctx;
    /* A chip staged against a link that never completed would leave the gate
     * BUSY for ever; clear it on the way out. */
    if (chipgate_busy(&s_gate)) {
        chipgate_clear(&s_gate);
    }
    settings_flush();   /* deferred favourites, written now that nothing clocks */
}

static bool mode_enter_multiboot(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx; (void)game; (void)has_game;
    /* No game applies to Send Program; the rescan and browser reset live in
     * gate_enter_mode. An empty drive is reported by the browser, not refused. */
    gate_enter_mode(APP_MODE_MULTIBOOT, CG_GAME_BN6_US);
    return true;
}

static void mode_exit_multiboot(void *ctx) {
    (void)ctx;
    s_send_status[0] = '\0';
    s_send_pct       = 0;
}

static bool mode_enter_chip_scan(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx; (void)has_game;
    gate_enter_mode(APP_MODE_CHIP_SCAN, game);
    return true;
}

static bool mode_enter_usb_storage(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx; (void)game; (void)has_game;
    s_exposed = false;   /* we own the volume on entry */
    return true;
}

static void mode_exit_usb_storage(void *ctx) {
    (void)ctx;
    /*
     * Reclaim the volume so the catalogue never reads a drive the host owns.
     * Leaving the mode is the deliberate "take it back" the screen describes;
     * inferring release from host activity is what corrupts a FAT volume.
     */
    if (s_exposed) {
        msc_claim_from_host();
        s_exposed = false;
    }
}

static bool mode_enter_selftest(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx; (void)has_game;
    gate_enter_mode(APP_MODE_SELFTEST, game);
    return true;
}

static void mode_exit_selftest(void *ctx) {
    (void)ctx;
    s_selftest = false;   /* stop the automatic chip sends */
}

static bool mode_enter_link(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx; (void)game; (void)has_game;
    /* The bridge is created by tasks.c; a build without it cannot link. */
    return tasks_bridge() != NULL;
}

/*
 * Link: Wireless is the GB-Link wireless cable on hardware. The word-level
 * bridge it used to run cannot keep a Gen 3 link alive (a relayed word arrives
 * after the slot it was for); GB-Link's packet relay can, and two boards play
 * Celio for each other. Pokemon Gen 3 by cable (GB-Link mode 0x01).
 */
static bool mode_enter_wireless(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx; (void)game; (void)has_game;
#if defined(GBAGATE_QEMU)
    return tasks_bridge() != NULL;
#else
    return wireless_cable_start(0x01, 0) == ESP_OK;
#endif
}

static void mode_exit_wireless(void *ctx) {
    (void)ctx;
#if !defined(GBAGATE_QEMU)
    wireless_cable_stop();
#endif
}

static void mode_tick_link(void *ctx) {
    (void)ctx;
    /*
     * The measurement behind the round-trip figure on the link screen. Nothing
     * else calls bridge_ping(), so without this tick rtt_us stays the zero it
     * initialised to and the screen cannot tell a usable link from a dead one.
     */
    link_bridge_t *b = tasks_bridge();
    if (b) {
        bridge_ping(b, (uint32_t)esp_timer_get_time());
    }
}

static bool mode_enter_settings(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx; (void)game; (void)has_game;
    return true;
}

static void mode_exit_settings(void *ctx) {
    (void)ctx;
    settings_flush();   /* deferred save, flushed as the running screen is left */
}

static bool mode_enter_diagnostics(void *ctx, cg_game_t game, bool has_game) {
    (void)ctx; (void)game; (void)has_game;
    return true;   /* reads live state only; nothing to set up or refuse */
}

static const app_mode_handler_t k_handlers[] = {
    { APP_MODE_CHIP_GATE,     mode_enter_chip_gate,   mode_exit_chip_gate,   NULL           },
    { APP_MODE_LINK_USB,      mode_enter_link,        NULL,                  mode_tick_link },
    { APP_MODE_LINK_WIRELESS, mode_enter_wireless,    mode_exit_wireless,    mode_tick_link },
    { APP_MODE_MULTIBOOT,     mode_enter_multiboot,   mode_exit_multiboot,   NULL           },
    { APP_MODE_CHIP_SCAN,     mode_enter_chip_scan,   NULL,                  NULL           },
    { APP_MODE_USB_STORAGE,   mode_enter_usb_storage, mode_exit_usb_storage, NULL           },
    { APP_MODE_SELFTEST,      mode_enter_selftest,    mode_exit_selftest,    NULL           },
    { APP_MODE_SETTINGS,      mode_enter_settings,    mode_exit_settings,    NULL           },
    { APP_MODE_DIAGNOSTICS,   mode_enter_diagnostics, NULL,                  NULL           },
};
#define HANDLER_COUNT (sizeof k_handlers / sizeof k_handlers[0])

void gate_shell_start(uint32_t caps) {
    shell_init(&s_shell, caps);
    /*
     * Bind the dispatcher to the handler table. caps is the same mask
     * shell_init() was given, which app_modes.h requires: the menu and the
     * dispatcher must agree about what each mode needs, or a mode lists but
     * refuses to start.
     */
    app_dispatch_init(&s_dispatch, k_handlers, HANDLER_COUNT, caps, NULL);
    s_shell_ready = true;
    ui_service(NULL, false);
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
