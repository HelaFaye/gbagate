/*
 * test_screens.c -- render every screen the device can show, and check them.
 *
 *   make run-screens          the checks
 *   make screens              also writes build/screens/: one .ppm per screen
 *                             and gallery.ppm, every screen side by side
 *
 * test_render covers the chip list. This covers the rest: the mode menu, the
 * game list, Settings, Send Program, and every mode's page, each in the states
 * worth seeing. They used to be drawn only on the device, from its globals, so
 * the first time anyone saw one was on a panel.
 *
 * The footers are the real control hints, bound the way the device binds them
 * (controls.c, for a rotary encoder), so a hint that crowds out the screen it
 * belongs to shows here too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chipgate.h"
#include "controls.h"
#include "hw_caps.h"
#include "settings.h"
#include "ui_font.h"
#include "ui_romview.h"
#include "ui_screens.h"
#include "ui_settings.h"
#include "ui_shell.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-44s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static uint16_t    g_fb[UI_SCREEN_W * UI_SCREEN_H];
static ui_canvas_t g_canvas = { g_fb, UI_SCREEN_W, UI_SCREEN_H };
static uint16_t px(int x, int y) { return g_fb[(size_t)y * UI_SCREEN_W + x]; }

/* ---- output ------------------------------------------------------------ */

static const char *g_out;    /* directory for .ppm files, or NULL */

#define GAL_COLS   4
#define GAL_MAX    40
#define GAL_SCALE  2
#define GAL_CAP_H  14
#define GAL_PAD    8
#define GAL_CELL_W (UI_SCREEN_W * GAL_SCALE + GAL_PAD)
#define GAL_CELL_H (UI_SCREEN_H * GAL_SCALE + GAL_CAP_H + GAL_PAD)

static uint16_t g_shots[GAL_MAX][UI_SCREEN_W * UI_SCREEN_H];
static char     g_names[GAL_MAX][40];
static int      g_nshots;

static void rgb(uint16_t c, uint8_t out[3]) {
    out[0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
    out[1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
    out[2] = (uint8_t)((c & 0x1F) * 255 / 31);
}

/* Keep the frame for the gallery, and write it out on its own. */
static void shot(const char *name) {
    if (g_nshots < GAL_MAX) {
        memcpy(g_shots[g_nshots], g_fb, sizeof g_fb);
        snprintf(g_names[g_nshots], sizeof g_names[0], "%s", name);
        g_nshots++;
    }
    if (!g_out) {
        return;
    }
    char path[256];
    snprintf(path, sizeof path, "%s/%s.ppm", g_out, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", UI_SCREEN_W, UI_SCREEN_H);
    for (int i = 0; i < UI_SCREEN_W * UI_SCREEN_H; i++) {
        uint8_t c[3];
        rgb(g_fb[i], c);
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}

/*
 * Every screen at twice size, captioned in the device's own font. Drawn
 * through a canvas over the gallery, so the caption is the same glyphs the
 * panel uses.
 */
static void write_gallery(void) {
    if (!g_out || g_nshots == 0) {
        return;
    }
    int rows = (g_nshots + GAL_COLS - 1) / GAL_COLS;
    int w = GAL_COLS * GAL_CELL_W + GAL_PAD, h = rows * GAL_CELL_H + GAL_PAD;
    uint16_t *gal = malloc((size_t)w * h * sizeof *gal);
    if (!gal) return;
    ui_canvas_t gc = { gal, (uint16_t)w, (uint16_t)h };
    ui_fill(&gc, 0, 0, w, h, RGB565(8, 10, 16));
    for (int n = 0; n < g_nshots; n++) {
        int x0 = GAL_PAD + (n % GAL_COLS) * GAL_CELL_W;
        int y0 = GAL_PAD + (n / GAL_COLS) * GAL_CELL_H;
        ui_text(&gc, x0, y0 + 3, g_names[n], COL_TEXT_DIM);
        for (int y = 0; y < UI_SCREEN_H * GAL_SCALE; y++) {
            for (int x = 0; x < UI_SCREEN_W * GAL_SCALE; x++) {
                gal[(size_t)(y0 + GAL_CAP_H + y) * w + x0 + x] =
                    g_shots[n][(y / GAL_SCALE) * UI_SCREEN_W + x / GAL_SCALE];
            }
        }
    }
    char path[256];
    snprintf(path, sizeof path, "%s/gallery.ppm", g_out);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t c[3];
            rgb(gal[i], c);
            fwrite(c, 1, 3, f);
        }
        fclose(f);
    }
    free(gal);
}

/* ---- text that does not fit --------------------------------------------- */

/*
 * Titles, footers and menu labels clipped on the panel. Listed rather than
 * failed while the screens are being reworked: the page lines above already
 * fail, and this list is what the rework works through. g_clipped is what the
 * summary line reports.
 */
static int g_clipped;

static void fits(const char *screen, const char *what, const char *text, int w) {
    if (text && ui_font_fit(text, (uint16_t)w) < strlen(text)) {
        printf("  clipped %s %s: \"%s\"\n", screen, what, text);
        g_clipped++;
    }
}

#define FOOTER_W (UI_SCREEN_W - 4)
#define LABEL_W  (UI_SCREEN_W - 16)   /* ui_render_menu: x 12, 4px right */

static void menu_fits(const char *screen, ui_label_fn label, void *ctx,
                      uint8_t count, const char *footer) {
    for (uint8_t i = 0; i < count; i++) {
        fits(screen, "row", label(ctx, i), LABEL_W);
    }
    fits(screen, "footer", footer, FOOTER_W);
}

/* ---- what every screen must get right ---------------------------------- */

/* Any pixel of this colour in the rectangle? */
static bool has_colour(int x, int y, int w, int h, uint16_t colour) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if (px(xx, yy) == colour) return true;
    return false;
}

static void check_frame(const char *name, bool footer) {
    char what[96];
    snprintf(what, sizeof what, "%s: header bar", name);
    check(what, px(UI_SCREEN_W - 2, 1), COL_HEADER);
    snprintf(what, sizeof what, "%s: title has ink", name);
    check(what, has_colour(3, 3, 60, UI_FONT_H, COL_BG), 1);
    if (footer) {
        snprintf(what, sizeof what, "%s: footer rule", name);
        check(what, px(10, UI_SCREEN_H - UI_FOOTER_H), COL_HEADER);
    }
}

/* Render a page, check it, keep it. Every line must fit: one that does not
 * is cut off on the panel, and is reported by name. */
static void page(const char *name, const ui_page_t *p) {
    ui_render_page(&g_canvas, p);
    fits(name, "title", p->title, UI_SCREEN_W - 6);
    fits(name, "footer", p->footer, FOOTER_W);
    check_frame(name, true);
    for (uint8_t i = 0; i < p->count; i++) {
        if (!ui_page_line_fits(p, i)) {
            printf("  FAIL %s: line %u does not fit: \"%s\"\n", name, i, p->line[i]);
            g_fail++;
        }
        char what[96];
        snprintf(what, sizeof what, "%s: line %u drawn", name, i);
        if (p->line[i][0]) {
            check(what, has_colour(2, UI_LIST_Y + 4 + i * 11, UI_SCREEN_W - 4,
                                   UI_FONT_H, p->colour[i]), 1);
        }
    }
    shot(name);
}

/* ---- controls, as the device binds them -------------------------------- */

static ui_input_t g_in;
static controls_t g_ctl;
static uint32_t no_buttons(void *ctx) { (void)ctx; return 0; }

#define CTRLS_PAGE (CTRL_BIT(CTRL_CONFIRM) | CTRL_BIT(CTRL_CANCEL))
#define CTRLS_LIST (CTRLS_PAGE | CTRL_BIT(CTRL_PREV) | CTRL_BIT(CTRL_NEXT) | \
                    CTRL_BIT(CTRL_PAGE_PREV) | CTRL_BIT(CTRL_PAGE_NEXT))

static const char *hint(uint32_t wanted) {
    controls_bind(&g_ctl, &g_in, wanted);
    return controls_hint(&g_ctl);
}

/* ---- the menus --------------------------------------------------------- */

static const char *shell_label(void *ctx, uint8_t i) {
    return shell_label_at((ui_shell_t *)ctx, i);
}

static void test_shell(void) {
    printf("shell\n");
    ui_shell_t s;
    shell_init(&s, 0xFFFFFFFFu);
    ui_render_menu(&g_canvas, "gbagate", shell_label, &s, shell_item_count(&s),
                   shell_cursor(&s), "gate");
    menu_fits("menu", shell_label, &s, shell_item_count(&s), "gate");
    check_frame("menu", true);
    shot("menu");

    shell_select(&s);   /* Battle Chip: pick a game first */
    check("Battle Chip asks for a game", s.screen, SHELL_SCREEN_GAMES);
    ui_render_menu(&g_canvas, "Choose game", shell_label, &s, shell_item_count(&s),
                   shell_cursor(&s), "back: hold");
    menu_fits("games", shell_label, &s, shell_item_count(&s), "back: hold");
    check_frame("games", true);
    shot("games");

    /* A refused mode: the reason replaces the variant name in the footer. */
    shell_back(&s);
    ui_render_menu(&g_canvas, "gbagate", shell_label, &s, shell_item_count(&s),
                   shell_cursor(&s), "no handler");
    shot("menu-refused");

    /* Only what this board has: no WiFi, no reader, no USB. */
    shell_init(&s, HW_CAP_DISPLAY | HW_CAP_LINK_PORT);
    ui_render_menu(&g_canvas, "gbagate", shell_label, &s, shell_item_count(&s),
                   shell_cursor(&s), "mini");
    shot("menu-minimal");
}

static void test_settings(void) {
    printf("settings\n");
    settings_t st;
    settings_defaults(&st);
    ui_settings_t v;
    uiset_init(&v, &st);
    ui_render_menu(&g_canvas, "Settings", uiset_label, &v, uiset_count(),
                   v.cursor, uiset_footer(&v));
    menu_fits("settings", uiset_label, &v, uiset_count(), uiset_footer(&v));
    check_frame("settings", true);
    shot("settings");
    uiset_toggle_edit(&v);
    ui_render_menu(&g_canvas, "Settings", uiset_label, &v, uiset_count(),
                   v.cursor, uiset_footer(&v));
    shot("settings-edit");
}

static void test_send_program(void) {
    printf("send program\n");
    static rom_catalog_t cat;
    memset(&cat, 0, sizeof cat);
    const struct { const char *n; rom_status_t s; } roms[] = {
        { "tester.gba", ROM_OK }, { "pokemon-link.mb", ROM_OK },
        { "too-big.gba", ROM_BAD_SIZE }, { "patched.gba", ROM_BAD_LOGO },
    };
    for (unsigned i = 0; i < sizeof roms / sizeof roms[0]; i++) {
        snprintf(cat.entries[i].name, ROM_NAME_MAX, "%s", roms[i].n);
        cat.entries[i].size = cat.entries[i].effective = 0x8000;
        cat.entries[i].status = roms[i].s;
        cat.playable += roms[i].s == ROM_OK;
    }
    cat.count = 4;

    romview_t v;
    romview_init(&v);
    romview_set(&v, &cat, ROMVIEW_OK);
    ui_render_menu(&g_canvas, "Send Program", romview_label, &v,
                   romview_count(&v), (uint8_t)v.cursor, romview_hint(&v));
    menu_fits("send-list", romview_label, &v, romview_count(&v), romview_hint(&v));
    check_frame("send", true);
    shot("send-list");
    romview_move(&v, 3);
    ui_render_menu(&g_canvas, "Send Program", romview_label, &v,
                   romview_count(&v), (uint8_t)v.cursor, romview_hint(&v));
    shot("send-list-bad");

    const romview_state_t empty[] = { ROMVIEW_HOST_OWNS, ROMVIEW_NO_STORAGE, ROMVIEW_EMPTY };
    const char *names[] = { "send-host-owns", "send-no-storage", "send-empty" };
    for (int i = 0; i < 3; i++) {
        romview_init(&v);
        romview_set(&v, NULL, empty[i]);
        ui_render_notice(&g_canvas, "Send Program", romview_empty_text(&v));
        fits(names[i], "text", romview_empty_text(&v), UI_SCREEN_W - 4);
        check_frame(names[i], false);
        check(names[i], has_colour(2, UI_LIST_Y + 20, UI_SCREEN_W - 4, UI_FONT_H,
                                   COL_TEXT), 1);
        shot(names[i]);
    }

    /* The bar must say how far, not just that something is happening. */
    ui_render_progress(&g_canvas, "Send Program", "tester.gba", "power on the GBA",
                       40, hint(CTRLS_PAGE));
    fits("send-progress", "footer", hint(CTRLS_PAGE), FOOTER_W);
    check_frame("progress", true);
    int bw = UI_SCREEN_W - 8, end = 4 + bw * 40 / 100;
    check("progress: filled to 40%", px(end - 1, UI_LIST_Y + 44), COL_BAR_THUMB);
    check("progress: track past 40%", px(end + 1, UI_LIST_Y + 44), COL_BAR_TRACK);
    shot("send-progress");
    ui_render_progress(&g_canvas, "Send Program", "tester.gba", "sent", 250,
                       hint(CTRLS_PAGE));
    check("progress: over 100% clamps", px(4 + bw - 1, UI_LIST_Y + 44), COL_BAR_THUMB);
}

/* ---- the pages --------------------------------------------------------- */

static void test_pages(void) {
    printf("pages\n");
    const char *foot = hint(CTRLS_PAGE);
    ui_page_t p;

    ui_diag_t d = { .caps = HW_CAP_DISPLAY | HW_CAP_LINK_PORT | HW_CAP_USB,
                    .linked = true, .transfers = 123456, .chips_sent = 12,
                    .gate_id = 0x00C6, .ui_dropped = 0 };
    ui_page_diagnostics(&p, "Diagnostics", foot, &d);
    page("diagnostics", &p);

    ui_page_refused(&p, "Infrared", foot, "no handler");
    page("refused", &p);

    link_bridge_t b;
    memset(&b, 0, sizeof b);
    ui_page_link_usb(&p, "Link: USB", foot, &b);
    page("usb-down", &p);
    b.state = BRIDGE_READY; b.words_sent = 48211; b.words_received = 48190; b.rtt_us = 840;
    ui_page_link_usb(&p, "Link: USB", foot, &b);
    page("usb-ready", &p);

    gbl_peer_t peer;
    memset(&peer, 0, sizeof peer);
    peer.mode = 0x01;
    peer.state = GBL_PEER_SEARCHING;
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, NULL, "Gen 3 cable");
    page("wireless-searching", &p);
    peer.mode_mismatch = 3;
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, NULL, "Gen 3 cable");
    page("wireless-other-mode", &p);
    const uint8_t mac[6] = { 0x24, 0x6F, 0x28, 0xA1, 0x0B, 0x3C };
    peer.state = GBL_PEER_LINKED; peer.master = true;
    peer.data_out = 1840; peer.data_in = 1838; peer.retransmits = 4;
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, mac, "Gen 3 cable");
    page("wireless-linked", &p);
    peer.mode = 0x07; peer.rfu.seen = true; peer.rfu.transfers = 900;
    peer.rfu.any_rx = true; peer.rfu.found = true; peer.rfu.resets = 1;
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, mac, "Wireless Adapter");
    page("wireless-adapter", &p);

    ui_ir_t ir = { .ready = true, .commands = 17, .aborted = 2, .last = 0x123 };
    ui_page_infrared(&p, "Infrared", foot, &ir);
    page("infrared", &p);

    ui_scan_t sc = { .reader = true, .advance = true, .id = 0x0101 };
    ui_page_chip_scan(&p, "Read Chip", foot, &sc);
    page("read-chip", &p);
    sc.id = CG_CHIP_NONE;
    ui_page_chip_scan(&p, "Read Chip", foot, &sc);
    page("read-chip-unreadable", &p);
    memset(&sc, 0, sizeof sc);
    ui_page_chip_scan(&p, "Read Chip", foot, &sc);
    page("read-chip-no-reader", &p);

    ui_page_usb_storage(&p, "USB Storage", foot, false);
    page("storage-ours", &p);
    ui_page_usb_storage(&p, "USB Storage", foot, true);
    page("storage-pcs", &p);

    ui_page_status(&p, "Self Test", foot, "SELF TEST");
    page("placeholder", &p);

    ui_render_no_interface(&g_canvas);
    check_frame("no-interface", false);
    shot("no-interface");
}

/* ---- the page model itself --------------------------------------------- */

static void test_page_model(void) {
    printf("page model\n");
    ui_page_t p;
    ui_page_init(&p, "T", "F");
    for (int i = 0; i < UI_PAGE_LINES + 3; i++) {
        ui_page_add(&p, COL_TEXT, "line %d", i);
    }
    check("lines past the limit are dropped", p.count, UI_PAGE_LINES);
    ui_page_init(&p, "T", "F");
    /* 2px margins leave 124px: 20 glyphs (119px), not 21 (125px). */
    ui_page_add(&p, COL_TEXT, "%s", "12345678901234567890");
    ui_page_add(&p, COL_TEXT, "%s", "123456789012345678901");
    check("20 glyphs fit", ui_page_line_fits(&p, 0), 1);
    check("21 glyphs do not", ui_page_line_fits(&p, 1), 0);

    ui_page_init(&p, "T", "F");
    ui_page_add_wrapped(&p, COL_TEXT, "display, USB, link cable, WiFi, reader");
    check("wrapped into lines", p.count, 2);
    check("wrapped at a space", strcmp(p.line[0], "display, USB, link"), 0);
    check("rest on the next line", strcmp(p.line[1], "cable, WiFi, reader"), 0);
    ui_page_init(&p, "T", "F");
    ui_page_add_wrapped(&p, COL_TEXT, "abcdefghijklmnopqrstuvwxyz");
    check("one long word is cut, not looped", p.count, 2);
    check("its rest follows", strcmp(p.line[1], "uvwxyz"), 0);
    ui_page_init(&p, NULL, NULL);
    check("NULL title is empty", p.title[0], 0);
}

int main(int argc, char **argv) {
    /* argv[1] is the chip database every suite is handed; argv[2], if given,
     * is where to write the screens. */
    g_out = argc > 2 ? argv[2] : NULL;
    ui_input_init(&g_in, INPUT_ENCODER, no_buttons, NULL);

    test_page_model();
    test_shell();
    test_settings();
    test_send_program();
    test_pages();
    write_gallery();

    if (g_fail) {
        printf("%d FAILURES\n", g_fail);
        return 1;
    }
    printf("all checks passed (%d screens, %d clipped strings)\n", g_nshots, g_clipped);
    return 0;
}
