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
 * The footers are built the way the device builds them -- each page naming
 * what its keys do, for an encoder -- so a footer that crowds out the screen
 * it belongs to fails here.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applet.h"
#include "chipgate.h"
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
#define GAL_MAX    72
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
 * Titles, footers and menu rows clipped on the panel. A failure: the rework
 * took the list from 23 to none, and a new screen or string must not add one.
 */
static void fits(const char *screen, const char *what, const char *text, int w) {
    if (text && ui_font_fit(text, (uint16_t)w) < strlen(text)) {
        printf("  FAIL clipped %s %s: \"%s\"\n", screen, what, text);
        g_fail++;
    }
}

#define FOOTER_W (UI_SCREEN_W - 4)
#define LABEL_W  (UI_SCREEN_W - 16)   /* ui_render_menu: x 12, 4px right */

/* A "left\tright" footer fits when both halves and the gap do. */
static void footer_fits(const char *screen, const char *footer) {
    if (!footer) return;
    const char *tab = strchr(footer, '\t');
    char left[64];
    size_t n = tab ? (size_t)(tab - footer) : strlen(footer);
    memcpy(left, footer, n);
    left[n] = '\0';
    int w = ui_font_width(left) + (tab && tab[1] ? ui_font_width(tab + 1) + 6 : 0);
    if (w > FOOTER_W) {
        printf("  FAIL clipped %s footer: \"%s\"\n", screen, footer);
        g_fail++;
    }
}

static void menu_fits(const char *screen, ui_label_fn label, void *ctx,
                      uint8_t count, const char *footer) {
    for (uint8_t i = 0; i < count; i++) {
        fits(screen, "row", label(ctx, i), LABEL_W);
    }
    footer_fits(screen, footer);
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
    check(what, has_colour(3, 3, 60, UI_FONT_H, COL_ON_HEADER), 1);
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
    if (!ui_page_footer_fits(p)) {
        printf("  FAIL %s: footer does not fit: \"%s\" + \"%s\"\n", name,
               p->footer, p->footer_right);
        g_fail++;
    }
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

    /* The link choice, on a board with both. */
    shell_init(&s, 0xFFFFFFFFu);
    for (uint8_t i = 0; i < shell_item_count(&s); i++) {
        if (!strcmp(shell_label_at(&s, i), "Link")) s.mode_cursor = i;
    }
    shell_select(&s);
    check("Link opens the choice", s.screen, SHELL_SCREEN_LINK);
    ui_render_menu(&g_canvas, shell_screen_title(&s), shell_label, &s,
                   shell_item_count(&s), shell_cursor(&s), "back: hold");
    menu_fits("link", shell_label, &s, shell_item_count(&s), "back: hold");
    check_frame("link", true);
    shot("link-choice");

    /* Only what this board has: no WiFi, no reader, no USB. */
    shell_init(&s, HW_CAP_DISPLAY | HW_CAP_LINK_PORT);
    ui_render_menu(&g_canvas, "gbagate", shell_label, &s, shell_item_count(&s),
                   shell_cursor(&s), "mini");
    shot("menu-minimal");
}

static void test_settings(void) {
    printf("settings\n");
    ui_keys_t k = ui_keys_for(0);
    settings_t st;
    settings_defaults(&st);
    ui_settings_t v;
    uiset_init(&v, &st);
    ui_render_menu(&g_canvas, "Settings", uiset_label, &v, uiset_count(),
                   v.cursor, uiset_footer(&v, &k));
    menu_fits("settings", uiset_label, &v, uiset_count(), uiset_footer(&v, &k));
    check_frame("settings", true);
    shot("settings");
    uiset_toggle_edit(&v);
    ui_render_menu(&g_canvas, "Settings", uiset_label, &v, uiset_count(),
                   v.cursor, uiset_footer(&v, &k));
    shot("settings-edit");
}

static void test_send_program(void) {
    printf("send program\n");
    ui_keys_t k = ui_keys_for(0);
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
                   romview_count(&v), (uint8_t)v.cursor, romview_hint(&v, &k));
    menu_fits("send-list", romview_label, &v, romview_count(&v), romview_hint(&v, &k));
    check_frame("send", true);
    shot("send-list");
    romview_move(&v, 3);
    ui_render_menu(&g_canvas, "Send Program", romview_label, &v,
                   romview_count(&v), (uint8_t)v.cursor, romview_hint(&v, &k));
    shot("send-list-bad");

    const romview_state_t empty[] = { ROMVIEW_HOST_OWNS, ROMVIEW_NO_STORAGE, ROMVIEW_EMPTY };
    const char *names[] = { "send-host-owns", "send-no-storage", "send-empty" };
    for (int i = 0; i < 3; i++) {
        romview_init(&v);
        romview_set(&v, NULL, empty[i]);
        ui_render_notice(&g_canvas, "Send Program", romview_empty_text(&v));
        /* Wrapped onto the panel: every line fits, and there are few. */
        ui_page_t w;
        ui_page_init(&w, NULL, NULL);
        ui_page_add_wrapped(&w, COL_TEXT, romview_empty_text(&v));
        check(names[i], w.count >= 1 && w.count <= 4, 1);
        for (uint8_t l = 0; l < w.count; l++) {
            check(names[i], ui_page_line_fits(&w, l), 1);
        }
        check_frame(names[i], false);
        check(names[i], has_colour(2, UI_LIST_Y + 20, UI_SCREEN_W - 4, UI_FONT_H,
                                   COL_TEXT), 1);
        shot(names[i]);
    }

    /* The bar must say how far, not just that something is happening. */
    ui_keys_t enc = ui_keys_for(INPUT_ENCODER);
    char foot[UI_PAGE_TEXT];
    ui_keys_hint(foot, sizeof foot, &enc, NULL);
    check("progress footer", strcmp(foot, "hold back"), 0);
    const char *advice = NULL;
    const char *status = ui_send_result_text(2, &advice);
    ui_render_progress(&g_canvas, "Send Program", "tester.gba", "waiting for the GBA",
                       "turn it on, no cart", 40, foot);
    fits("send-progress", "status", "waiting for the GBA", UI_SCREEN_W - 4);
    fits("send-progress", "advice", "turn it on, no cart", UI_SCREEN_W - 4);
    check_frame("progress", true);
    int bw = UI_SCREEN_W - 8, end = 4 + bw * 40 / 100;
    check("progress: filled to 40%", px(end - 1, UI_LIST_Y + 52), COL_BAR_THUMB);
    check("progress: track past 40%", px(end + 1, UI_LIST_Y + 52), COL_BAR_TRACK);
    check("progress: advice drawn", has_colour(2, UI_LIST_Y + 31, 120, UI_FONT_H, COL_TEXT), 1);
    shot("send-progress");
    ui_render_progress(&g_canvas, "Send Program", "tester.gba", status, advice, 3, foot);
    shot("send-no-gba");
    ui_render_progress(&g_canvas, "Send Program", "tester.gba", "sent", "", 250, foot);
    check("progress: over 100% clamps", px(4 + bw - 1, UI_LIST_Y + 52), COL_BAR_THUMB);

    /* Every result reads as a player would say it, with something to do. */
    for (int r = 0; r <= 6; r++) {
        const char *a = NULL;
        const char *t = ui_send_result_text(r, &a);
        char what[40];
        snprintf(what, sizeof what, "result %d", r);
        check(what, t && t[0] && a != NULL, 1);
        fits(what, "text", t, UI_SCREEN_W - 4);
        fits(what, "advice", a, UI_SCREEN_W - 4);
    }
    check("advice is optional", ui_send_result_text(0, NULL) != NULL, 1);
}

/* ---- the pages --------------------------------------------------------- */

/* The page's first line, and its colour: the state a player reads first. */
static void lead(const char *name, const ui_page_t *p, const char *text, uint16_t colour) {
    char what[96];
    snprintf(what, sizeof what, "%s: says \"%s\"", name, text);
    check(what, strcmp(p->line[0], text), 0);
    snprintf(what, sizeof what, "%s: in its status colour", name);
    check(what, p->colour[0], colour);
}

static void footer_is(const char *name, const ui_page_t *p, const char *left, const char *right) {
    char what[96];
    snprintf(what, sizeof what, "%s: footer \"%s\"", name, left);
    check(what, strcmp(p->footer, left), 0);
    snprintf(what, sizeof what, "%s: back \"%s\"", name, right);
    check(what, strcmp(p->footer_right, right), 0);
}

static void test_pages(void) {
    printf("pages\n");
    ui_keys_t k = ui_keys_for(INPUT_ENCODER);
    const ui_keys_t *foot = &k;
    ui_page_t p;

    check("encoder keys", strcmp(k.confirm, "click") == 0 && strcmp(k.back, "hold") == 0, 1);
    ui_keys_t ab = ui_keys_for(INPUT_DPAD4);
    check("button keys", strcmp(ab.confirm, "A") == 0 && strcmp(ab.back, "B") == 0, 1);
    ui_page_init(&p, "T", NULL);
    ui_page_keys(&p, NULL, "x");
    check("no keys, no footer", p.footer[0] || p.footer_right[0], 0);

    ui_diag_t d = { .caps = HW_CAP_DISPLAY | HW_CAP_LINK_PORT | HW_CAP_USB,
                    .linked = true, .transfers = 123456, .chips_sent = 12,
                    .gate_id = 0x00C6, .ui_dropped = 0 };
    ui_page_diagnostics(&p, "Diagnostics", foot, &d);
    footer_is("diagnostics", &p, "", "hold back");
    page("diagnostics", &p);

    const struct { int st; const char *name; } refusals[] = {
        { 1, "refused-no-handler" }, { 2, "refused-hardware" }, { 3, "refused-game" },
        { 4, "refused-busy" }, { 5, "refused-by-mode" },
    };
    for (unsigned i = 0; i < sizeof refusals / sizeof refusals[0]; i++) {
        ui_page_refused(&p, "Infrared", foot, refusals[i].st);
        lead(refusals[i].name, &p, "did not start", COL_BAD);
        check(refusals[i].name, p.count >= 2, 1);
        page(refusals[i].name, &p);
    }

    link_bridge_t b;
    memset(&b, 0, sizeof b);
    ui_page_link_usb(&p, "Link: USB", foot, &b);
    lead("usb-down", &p, "waiting for the PC", COL_WARN);
    page("usb-down", &p);
    b.state = BRIDGE_HELLO;
    ui_page_link_usb(&p, "Link: USB", foot, &b);
    lead("usb-hello", &p, "PC found", COL_WARN);
    page("usb-hello", &p);
    b.state = BRIDGE_READY; b.words_sent = 48211; b.words_received = 48190;
    ui_page_link_usb(&p, "Link: USB", foot, &b);
    check("usb: measuring before a ping", strcmp(p.line[1], "measuring delay"), 0);
    b.rtt_us = 840;
    ui_page_link_usb(&p, "Link: USB", foot, &b);
    lead("usb-ready", &p, "connected to PC", COL_GOOD);
    check("usb: delay in ms", strcmp(p.line[1], "delay 0.8 ms good"), 0);
    page("usb-ready", &p);
    b.rtt_us = 4200;
    ui_page_link_usb(&p, "Link: USB", foot, &b);
    check("usb: 4 ms is ok", p.colour[1], COL_WARN);
    b.rtt_us = 31000;
    ui_page_link_usb(&p, "Link: USB", foot, &b);
    check("usb: 31 ms is slow", strcmp(p.line[1], "delay 31.0 ms slow"), 0);
    check("usb: in red", p.colour[1], COL_BAD);
    page("usb-slow", &p);
    ui_page_link_usb(&p, "Link: USB", foot, NULL);
    lead("usb-none", &p, "not available", COL_BAD);
    page("usb-none", &p);

    gbl_peer_t peer;
    memset(&peer, 0, sizeof peer);
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, NULL, "Gen 3 cable");
    check("wireless idle", strcmp(p.line[0], "starting..."), 0);
    peer.mode = 0x01;
    peer.state = GBL_PEER_SEARCHING;
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, NULL, "Gen 3 cable");
    lead("wireless-searching", &p, "looking for partner", COL_WARN);
    page("wireless-searching", &p);
    peer.mode_mismatch = 3;
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, NULL, "Gen 3 cable");
    check("wireless: says the other is set differently",
          strstr(p.line[2], "different air link") != NULL, 1);
    page("wireless-other-mode", &p);
    peer.state = GBL_PEER_LINKING;
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, NULL, "Gen 3 cable");
    lead("wireless-linking", &p, "partner found", COL_WARN);
    page("wireless-linking", &p);
    const uint8_t mac[6] = { 0x24, 0x6F, 0x28, 0xA1, 0x0B, 0x3C };
    peer.state = GBL_PEER_LINKED; peer.master = true;
    peer.tx_datagrams = 2000; peer.retransmits = 40;     /* 2%: good */
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, mac, "Gen 3 cable");
    lead("wireless-linked", &p, "linked to partner", COL_GOOD);
    check("wireless: carrying", strcmp(p.line[1], "as Gen 3 cable"), 0);
    check("wireless: signal good", strcmp(p.line[2], "signal good"), 0);
    check("wireless: partner id", strcmp(p.line[3], "partner ..0B3C"), 0);
    page("wireless-linked", &p);
    peer.retransmits = 200;                               /* 10%: fair */
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, mac, "Gen 3 cable");
    check("wireless: signal fair", strcmp(p.line[2], "signal fair"), 0);
    peer.retransmits = 600;                               /* 30%: poor */
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, mac, "Gen 3 cable");
    check("wireless: signal poor", p.colour[2], COL_BAD);
    page("wireless-poor", &p);
    peer.tx_datagrams = 5;
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, mac, "Gen 3 cable");
    check("wireless: no verdict on 5 datagrams", strncmp(p.line[2], "signal", 6) != 0, 1);
    peer.mode = 0x07; peer.rfu.seen = true; peer.rfu.transfers = 900;
    peer.rfu.any_rx = true; peer.rfu.found = true; peer.tx_datagrams = 900; peer.retransmits = 9;
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, mac, "Wireless Adapter");
    check("wireless: the adapter's GBA", strcmp(p.line[3], "GBA: found adapter"), 0);
    page("wireless-adapter", &p);
    peer.rfu.found = false; peer.rfu.any_rx = false;
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, mac, "Wireless Adapter");
    check("wireless: wrong cable", strcmp(p.line[3], "GBA: use GBC cable"), 0);
    peer.state = GBL_PEER_CLOSED;
    ui_page_wireless(&p, "Link: Wireless", foot, &peer, NULL, "Gen 3 cable");
    lead("wireless-closed", &p, "session ended", COL_WARN);
    check("closed: no stale adapter telemetry", p.count, 2);
    page("wireless-closed", &p);

    ui_ir_t ir = { .ready = false };
    ui_page_infrared(&p, "Infrared", foot, &ir);
    lead("infrared-none", &p, "no IR LED fitted", COL_BAD);
    footer_is("infrared-none", &p, "", "hold back");
    page("infrared-none", &p);
    ir.ready = true;
    ui_page_infrared(&p, "Infrared", foot, &ir);
    lead("infrared-ready", &p, "IR ready", COL_GOOD);
    footer_is("infrared-ready", &p, "", "hold back");   /* nothing to resend */
    page("infrared-ready", &p);
    ir.commands = 17; ir.aborted = 2; ir.last = 0x123;
    ui_page_infrared(&p, "Infrared", foot, &ir);
    footer_is("infrared", &p, "click send", "hold back");
    check("infrared: garbled warned", p.colour[3], COL_WARN);
    page("infrared", &p);

    ui_scan_t sc = { .reader = true, .advance = true, .id = 0x0101, .name = "Cannon" };
    ui_page_chip_scan(&p, "Read Chip", foot, &sc);
    lead("read-chip", &p, "Cannon", COL_GOOD);
    footer_is("read-chip", &p, "click send", "hold back");
    page("read-chip", &p);
    sc.name = NULL;
    ui_page_chip_scan(&p, "Read Chip", foot, &sc);
    lead("read-chip-unnamed", &p, "chip read", COL_GOOD);
    sc.id = CG_CHIP_NONE;
    ui_page_chip_scan(&p, "Read Chip", foot, &sc);
    lead("read-chip-unreadable", &p, "can't read the chip", COL_WARN);
    footer_is("read-chip-unreadable", &p, "", "hold back");
    page("read-chip-unreadable", &p);
    sc.advance = false;
    ui_page_chip_scan(&p, "Read Chip", foot, &sc);
    lead("read-chip-empty", &p, "insert a chip", COL_WARN);
    page("read-chip-empty", &p);
    memset(&sc, 0, sizeof sc);
    ui_page_chip_scan(&p, "Read Chip", foot, &sc);
    lead("read-chip-no-reader", &p, "no reader board", COL_BAD);
    page("read-chip-no-reader", &p);

    ui_page_usb_storage(&p, "USB Storage", foot, false);
    footer_is("storage-ours", &p, "click lend", "hold back");
    page("storage-ours", &p);
    ui_page_usb_storage(&p, "USB Storage", foot, true);
    footer_is("storage-pcs", &p, "click claim", "hold back");
    lead("storage-pcs", &p, "drive is on the PC", COL_WARN);
    page("storage-pcs", &p);
    ui_page_usb_storage(&p, "USB Storage", &ab, true);
    footer_is("storage-pcs-buttons", &p, "A claim", "B back");
    page("storage-pcs-buttons", &p);

    ui_selftest_t st = { .linked = false };
    ui_page_selftest(&p, "Self Test", foot, &st);
    lead("selftest-waiting", &p, "waiting for the GBA", COL_WARN);
    page("selftest-waiting", &p);
    st.linked = true; st.transfers = 812; st.sent = 1;
    ui_page_selftest(&p, "Self Test", foot, &st);
    page("selftest-running", &p);
    st.sent = 3;
    ui_page_selftest(&p, "Self Test", foot, &st);
    lead("selftest-done", &p, "test chips sent", COL_GOOD);
    check("selftest: never more than 2 of 2", strcmp(p.line[1], "chips   2 of 2"), 0);
    page("selftest-done", &p);

    ui_page_status(&p, "Self Test", foot, "SELF TEST");
    page("placeholder", &p);

    ui_render_no_interface(&g_canvas);
    check_frame("no-interface", false);
    shot("no-interface");
}

/* ---- applets ------------------------------------------------------------ */

static void  *g_blob;
static size_t g_blob_len;

static bool open_db(cg_game_t game, chipdb_t *db) {
    (void)game;
    return g_blob && chipdb_open(db, g_blob, g_blob_len);
}

static void test_applet_screens(const char *db_path) {
    printf("applets\n");
    FILE *f = db_path ? fopen(db_path, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        g_blob = malloc((size_t)n);
        g_blob_len = fread(g_blob, 1, (size_t)n, f);
        fclose(f);
    }
    applet_env_t env = { .caps = 0xFFFFFFFFu, .has_game = true,
                         .game = CG_GAME_BN6_US, .hint = "hold back",
                         .open_db = open_db };
    applet_bind_env(&env);

    for (uint8_t i = 0; i < applet_count(); i++) {
        const applet_t *a = applet_at(i);
        check(a->name, a->enter ? a->enter(&env) : 1, 1);
        int frames = a == applet_for_mode((app_mode_t)APP_MODE_APPLET_FIRST) ? 4 : 2;
        for (int k = 0; k < frames; k++) {
            ui_fill(&g_canvas, 0, 0, UI_SCREEN_W, UI_SCREEN_H, 0);
            applet_render(a, &env, &g_canvas);
            char name[40];
            snprintf(name, sizeof name, "applet-%s-%d", a->name, k + 1);
            for (char *c = name; *c; c++) if (*c == ' ') *c = '-';
            shot(name);
            a->input(&env, APPLET_IN_CONFIRM, 0);
        }
        if (a->exit) a->exit();
    }
    applet_bind_env(NULL);
}

/* ---- themes ------------------------------------------------------------ */

/* WCAG relative luminance of an RGB565 colour, as the panel shows it. */
static double luminance(uint16_t c) {
    uint8_t v[3];
    rgb(c, v);
    double ch[3];
    for (int i = 0; i < 3; i++) {
        double x = v[i] / 255.0;
        ch[i] = x <= 0.03928 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
    }
    return 0.2126 * ch[0] + 0.7152 * ch[1] + 0.0722 * ch[2];
}

static double contrast(uint16_t a, uint16_t b) {
    double la = luminance(a), lb = luminance(b);
    return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

/*
 * Every pair a screen draws, at the WCAG thresholds: 4.5 for text that must
 * be read, 3 for secondary text and status colours. A 1.44" panel at arm's
 * length is a harder read than a monitor, so these are a floor.
 */
static void test_theme_contrast(void) {
    printf("theme contrast\n");
    for (int id = 0; id < UI_THEME_COUNT; id++) {
        const ui_theme_t *t = ui_theme_get((ui_theme_id_t)id);
        const struct { const char *what; uint16_t fg, bg; double min; } pairs[] = {
            { "text on bg",          t->text,      t->bg,      4.5 },
            { "text on row band",    t->text,      t->row_alt, 4.5 },
            { "dim on bg",           t->text_dim,  t->bg,      3.0 },
            { "dim on footer",       t->text_dim,  t->row_alt, 3.0 },
            { "title on header",     t->on_header, t->header,  4.5 },
            { "text on held row",    t->on_header, t->held,    3.0 },
            { "accent on footer",    t->accent,    t->row_alt, 3.0 },
            { "accent on bg",        t->accent,    t->bg,      3.0 },
            { "good on bg",          t->good,      t->bg,      3.0 },
            { "warn on bg",          t->warn,      t->bg,      3.0 },
            { "bad on bg",           t->bad,       t->bg,      3.0 },
            { "favourite star",      t->star_on,   t->bg,      3.0 },
            { "scroll thumb",        t->bar_thumb, t->bar_track, 2.0 },
        };
        for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
            double r = contrast(pairs[i].fg, pairs[i].bg);
            if (r < pairs[i].min) {
                printf("  FAIL %s: %s %.2f, want %.1f\n", t->name, pairs[i].what,
                       r, pairs[i].min);
                g_fail++;
            }
        }
    }
    /* The code letter keeps the games' element colours in every theme. */
    const uint8_t elems[] = { CHIP_ELEM_NONE, CHIP_ELEM_FIRE, CHIP_ELEM_AQUA,
                              CHIP_ELEM_ELEC, CHIP_ELEM_WOOD };
    for (unsigned i = 0; i < sizeof elems; i++) {
        double r = contrast(COL_ON_ELEMENT, ui_element_colour(elems[i]));
        if (r < 4.5) {
            printf("  FAIL code letter on element %u: %.2f\n", elems[i], r);
            g_fail++;
        }
    }

    check("out-of-range theme is Classic",
          ui_theme_get((ui_theme_id_t)99) == ui_theme_get(UI_THEME_CLASSIC), 1);
    ui_theme_set((ui_theme_id_t)99);
    check("setting one selects Classic", ui_theme_current(), UI_THEME_CLASSIC);
    check("unknown name", strcmp(ui_theme_name((ui_theme_id_t)99), "?"), 0);
}

/* The menu and a page in every theme, for the gallery, with the frame checked
 * in each: a theme that left a colour behind would show here. */
static void test_theme_screens(void) {
    printf("theme screens\n");
    for (int id = 0; id < UI_THEME_COUNT; id++) {
        ui_theme_set((ui_theme_id_t)id);
        char name[40];
        ui_shell_t s;
        shell_init(&s, 0xFFFFFFFFu);
        shell_move(&s, 2);
        ui_render_menu(&g_canvas, "gbagate", shell_label, &s, shell_item_count(&s),
                       shell_cursor(&s), "gate");
        snprintf(name, sizeof name, "theme-%s", ui_theme_name((ui_theme_id_t)id));
        check_frame(name, true);
        check(name, px(UI_SCREEN_W - 2, UI_SCREEN_H / 2), ui_theme()->bg);
        shot(name);

        ui_page_t p;
        ui_diag_t d = { .caps = HW_CAP_DISPLAY | HW_CAP_LINK_PORT, .linked = true,
                        .transfers = 4096, .chips_sent = 3, .gate_id = 0x00C6 };
        ui_keys_t k = ui_keys_for(INPUT_ENCODER);
        ui_page_diagnostics(&p, "Diagnostics", &k, &d);
        snprintf(name, sizeof name, "theme-%s-page", ui_theme_name((ui_theme_id_t)id));
        page(name, &p);
    }
    ui_theme_set(UI_THEME_CLASSIC);
}

/* ---- the shared text rules (ui_text.h) ---------------------------------- */

static void test_text_rules(void) {
    printf("text rules\n");
    char out[64];
    ui_two_col(out, sizeof out, "theme", "Classic", 18);
    check("two columns: padded to 18", strcmp(out, "theme      Classic"), 0);
    ui_two_col(out, sizeof out, "pokemon-link.mb", "32K", 18);
    /* 14 glyphs of room: 13 of the name and the '.' that marks the cut. */
    check("a long left is cut and marked", strcmp(out, "pokemon-link.. 32K"), 0);
    check("the right is whole", strcmp(out + strlen(out) - 3, "32K"), 0);
    check("and the row exact", (long)strlen(out), 18);
    ui_two_col(out, sizeof out, "x", "a-very-long-value-indeed", 18);
    check("a right longer than the row is kept", strstr(out, "a-very-long-value-indeed") != NULL, 1);
    ui_two_col(out, sizeof out, NULL, "v", 4);
    check("no left", strcmp(out, "   v"), 0);
    ui_two_col(out, sizeof out, "name", NULL, 6);
    check("no right", strncmp(out, "name", 4), 0);

    ui_footer_join(out, sizeof out, "click send", "hold back");
    check("join", strcmp(out, "click send\thold back"), 0);
    ui_footer_join(out, sizeof out, "only", NULL);
    check("join, no right: no tab", strcmp(out, "only"), 0);
    ui_footer_join(out, sizeof out, NULL, "right");
    check("join, no left", strcmp(out, "\tright"), 0);
    ui_key_verb(out, sizeof out, "A", NULL);
    check("no verb, nothing", out[0], 0);

    /* The menu footer draws the right half right-aligned and dim. */
    ui_shell_t s;
    shell_init(&s, HW_CAP_DISPLAY);
    ui_render_menu(&g_canvas, "gbagate", shell_label, &s, shell_item_count(&s), 0,
                   "click open\tgate");
    int fy = UI_SCREEN_H - UI_FOOTER_H + (UI_FOOTER_H - UI_FONT_H) / 2;
    check("left half in text", has_colour(2, fy, 59, UI_FONT_H, COL_TEXT), 1);
    check("right half at the right, dim",
          has_colour(UI_SCREEN_W - 2 - 23, fy, 23, UI_FONT_H, COL_TEXT_DIM), 1);
    check("nothing between", has_colour(64, fy, 30, UI_FONT_H, COL_TEXT) ||
          has_colour(64, fy, 30, UI_FONT_H, COL_TEXT_DIM), 0);

    /* The chip screen's status, coloured by what it means. */
    check("SENT is good", ui_status_colour("SENT Cannon"), COL_GOOD);
    check("LINKED is good", ui_status_colour("LINKED"), COL_GOOD);
    check("BUSY warns", ui_status_colour("BUSY"), COL_WARN);
    check("WAITING warns", ui_status_colour("WAITING"), COL_WARN);
    check("a stalled link is bad", ui_status_colour("LINK STALLED"), COL_BAD);
    check("anything else is text", ui_status_colour("SELF TEST"), COL_TEXT);
    check("NULL is text", ui_status_colour(NULL), COL_TEXT);
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

    test_page_model();
    test_text_rules();
    test_shell();
    test_settings();
    test_send_program();
    test_pages();
    test_applet_screens(argc > 1 ? argv[1] : NULL);
    test_theme_contrast();
    test_theme_screens();
    write_gallery();

    if (g_fail) {
        printf("%d FAILURES\n", g_fail);
        return 1;
    }
    printf("all checks passed (%d screens)\n", g_nshots);
    return 0;
}
