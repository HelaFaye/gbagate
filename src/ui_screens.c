/*
 * ui_screens.c -- see ui_screens.h
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "chipgate.h"
#include "hw_caps.h"
#include "ir_cdz.h"
#include "ui_font.h"
#include "ui_screens.h"

#define TEXT_X     2
#define TEXT_W     (UI_SCREEN_W - 2 * TEXT_X)
#define LINE_Y0    (UI_LIST_Y + 4)
#define LINE_PITCH 11

/* ---- the page ---------------------------------------------------------- */

void ui_page_init(ui_page_t *p, const char *title, const char *footer) {
    memset(p, 0, sizeof *p);
    snprintf(p->title, sizeof p->title, "%s", title ? title : "");
    snprintf(p->footer, sizeof p->footer, "%s", footer ? footer : "");
}

void ui_page_add(ui_page_t *p, uint16_t colour, const char *fmt, ...) {
    if (p->count >= UI_PAGE_LINES) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->line[p->count], sizeof p->line[0], fmt, ap);
    va_end(ap);
    p->colour[p->count] = colour;
    p->count++;
}

void ui_page_add_wrapped(ui_page_t *p, uint16_t colour, const char *text) {
    while (text && *text && p->count < UI_PAGE_LINES) {
        size_t n = ui_font_fit(text, TEXT_W);
        size_t len = strlen(text);
        if (n < len) {
            /* Back up to the last space that fits; a single word wider than
             * the panel is cut rather than looping. */
            size_t cut = n;
            while (cut > 0 && text[cut] != ' ') {
                cut--;
            }
            if (cut > 0) {
                n = cut;
            }
        }
        ui_page_add(p, colour, "%.*s", (int)n, text);
        text += n;
        while (*text == ' ') {
            text++;
        }
    }
}

bool ui_page_line_fits(const ui_page_t *p, uint8_t i) {
    if (i >= p->count) {
        return true;
    }
    return ui_font_fit(p->line[i], TEXT_W) == strlen(p->line[i]);
}

/* Header, background and footer: what every screen here shares. */
static void frame(const ui_canvas_t *c, const char *title, const char *footer) {
    ui_fill(c, 0, 0, UI_SCREEN_W, UI_SCREEN_H, COL_BG);
    ui_fill(c, 0, 0, UI_SCREEN_W, UI_HEADER_H, COL_HEADER);
    ui_text_clipped(c, 3, (UI_HEADER_H - UI_FONT_H) / 2, UI_SCREEN_W - 6,
                    title, COL_ON_HEADER);
    if (!footer) {
        return;
    }
    int fy = UI_SCREEN_H - UI_FOOTER_H;
    ui_fill(c, 0, fy, UI_SCREEN_W, UI_FOOTER_H, COL_ROW_ALT);
    ui_fill(c, 0, fy, UI_SCREEN_W, 1, COL_HEADER);
    ui_text_clipped(c, TEXT_X, fy + (UI_FOOTER_H - UI_FONT_H) / 2, TEXT_W,
                    footer, COL_TEXT_DIM);
}

void ui_render_page(const ui_canvas_t *c, const ui_page_t *p) {
    frame(c, p->title, p->footer);
    for (uint8_t i = 0; i < p->count; i++) {
        ui_text_clipped(c, TEXT_X, LINE_Y0 + i * LINE_PITCH, TEXT_W,
                        p->line[i], p->colour[i]);
    }
}

/* ---- the pages --------------------------------------------------------- */

/*
 * Diagnostics is the mode you reach for when nothing else works, so it reports
 * the things that are otherwise only visible in a serial log: which
 * capabilities were detected, whether the link ever moved, and whether the UI
 * is losing events.
 */
void ui_page_diagnostics(ui_page_t *p, const char *title, const char *footer,
                         const ui_diag_t *d) {
    char caps[64];
    hw_caps_describe(d->caps, caps, sizeof caps);
    ui_page_init(p, title, footer);
    ui_page_add(p, COL_TEXT_DIM, "detected:");
    ui_page_add_wrapped(p, COL_TEXT, caps);
    ui_page_add(p, COL_TEXT, "link   %s", d->linked ? "linked" : "waiting");
    ui_page_add(p, COL_TEXT, "xfers  %lu", (unsigned long)d->transfers);
    ui_page_add(p, COL_TEXT, "chips  %lu", (unsigned long)d->chips_sent);
    ui_page_add(p, COL_TEXT, "gate   %04X", d->gate_id);
    ui_page_add(p, COL_TEXT, "uidrop %lu", (unsigned long)d->ui_dropped);
}

/*
 * Naming the reason matters: this page used to read "no screen for this mode
 * yet", which describes the firmware rather than the fault the user just hit.
 */
void ui_page_refused(ui_page_t *p, const char *title, const char *footer,
                     const char *reason) {
    ui_page_init(p, title, footer);
    ui_page_add(p, COL_TEXT, "mode did not start");
    ui_page_add(p, COL_TEXT_DIM, "%s", reason ? reason : "");
}

void ui_page_status(ui_page_t *p, const char *title, const char *footer,
                    const char *status) {
    ui_page_init(p, title, footer);
    ui_page_add(p, COL_TEXT, "%s", status ? status : "");
    ui_page_add(p, COL_TEXT_DIM, "no screen for this");
    ui_page_add(p, COL_TEXT_DIM, "mode yet");
}

void ui_page_link_usb(ui_page_t *p, const char *title, const char *footer,
                      const link_bridge_t *b) {
    ui_page_init(p, title, footer);
    ui_page_add(p, COL_TEXT, "%s", b ? bridge_state_name(b->state) : "no bridge");
    ui_page_add(p, COL_TEXT_DIM, "sent   %lu", b ? (unsigned long)b->words_sent : 0ul);
    ui_page_add(p, COL_TEXT_DIM, "recvd  %lu", b ? (unsigned long)b->words_received : 0ul);
    /* Round trip is the number that says whether a link is usable: under a
     * millisecond is fine, tens are not. */
    ui_page_add(p, COL_TEXT_DIM, "rtt    %lu us", b ? (unsigned long)b->rtt_us : 0ul);
}

/*
 * The wireless link cable: GB-Link's relay, run against another board over
 * ESP-NOW (src/gbl_peer.c). Retransmits climbing while frames still flow is a
 * noisy channel; frames stopping is the peer gone.
 */
void ui_page_wireless(ui_page_t *p, const char *title, const char *footer,
                      const gbl_peer_t *peer, const uint8_t *mac,
                      const char *air_link) {
    ui_page_init(p, title, footer);
    ui_page_add(p, COL_TEXT, "%s%s", gbl_peer_state_name(peer->state),
                peer->state >= GBL_PEER_SETTLING ? (peer->master ? " (M)" : " (S)") : "");
    if (mac) {
        ui_page_add(p, COL_TEXT_DIM, "%02X%02X%02X%02X%02X%02X",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else if (peer->mode_mismatch && peer->state == GBL_PEER_SEARCHING) {
        /* A board is there but set to another air link. */
        ui_page_add(p, COL_TEXT_DIM, "peer: other link");
    } else {
        ui_page_add(p, COL_TEXT_DIM, "%s: no peer", air_link ? air_link : "");
    }
    if (peer->mode == 0x07) {
        /* Wireless Adapter: whether the game has found the adapter is the
         * first question; the firmware's own telemetry answers it. */
        const char *gba = !peer->rfu.seen      ? "GBA: --"
                        : peer->rfu.found      ? "GBA: found adapter"
                        : !peer->rfu.transfers ? "GBA: no clock"
                        : !peer->rfu.any_rx    ? "GBA: use GBC cable"
                                               : "GBA: looking";
        ui_page_add(p, COL_TEXT_DIM, "%s", gba);
        ui_page_add(p, COL_TEXT_DIM, "out %lu in %lu",
                    (unsigned long)peer->data_out, (unsigned long)peer->data_in);
        ui_page_add(p, COL_TEXT_DIM, "resets %u", peer->rfu.resets);
    } else {
        ui_page_add(p, COL_TEXT_DIM, "out %lu in %lu",
                    (unsigned long)peer->data_out, (unsigned long)peer->data_in);
        ui_page_add(p, COL_TEXT_DIM, "resent %lu", (unsigned long)peer->retransmits);
    }
}

void ui_page_infrared(ui_page_t *p, const char *title, const char *footer,
                      const ui_ir_t *ir) {
    ui_page_init(p, title, footer);
    ui_page_add(p, COL_TEXT, "%s", ir->ready ? "IR ready" : "no IR fitted");
    ui_page_add(p, COL_TEXT_DIM, "cmds   %lu", (unsigned long)ir->commands);
    ui_page_add(p, COL_TEXT_DIM, "last   %03X  %X/%X/%X", (unsigned)ir->last,
                cdz_category(ir->last), cdz_motion(ir->last), cdz_action(ir->last));
    /*
     * Aborted trains is the number that tells you whether the timing is right.
     * A decoder measuring the wrong thing aborts nearly everything while still
     * looking alive, so a bare command count would not show it.
     */
    ui_page_add(p, COL_TEXT_DIM, "aborted %lu", (unsigned long)ir->aborted);
}

void ui_page_chip_scan(ui_page_t *p, const char *title, const char *footer,
                       const ui_scan_t *s) {
    ui_page_init(p, title, footer);
    ui_page_add(p, COL_TEXT, "%s", s->reader ? "reader ready" : "no reader");
    ui_page_add(p, COL_TEXT_DIM, "advance %s", s->advance ? "chip in" : "empty");
    ui_page_add(p, COL_TEXT_DIM, "beast   %s", s->beast ? "chip in" : "empty");
    if (s->reader && (s->advance || s->beast)) {
        if (s->id != CG_CHIP_NONE) {
            ui_page_add(p, COL_TEXT_DIM, "id     %04X", s->id);
        } else {
            ui_page_add(p, COL_TEXT_DIM, "unreadable");
        }
    }
}

void ui_page_selftest(ui_page_t *p, const char *title, const char *footer,
                      const ui_selftest_t *t) {
    ui_page_init(p, title, footer);
    if (!t->linked) {
        ui_page_add(p, COL_WARN, "waiting for the GBA");
        ui_page_add(p, COL_TEXT_DIM, "run tester.gba on it");
        ui_page_add(p, COL_TEXT_DIM, "(Send Program can)");
        return;
    }
    bool done = t->sent >= 2;
    ui_page_add(p, done ? COL_GOOD : COL_TEXT, "%s",
                done ? "test chips sent" : "testing...");
    ui_page_add(p, COL_TEXT_DIM, "chips   %lu of 2", (unsigned long)(t->sent > 2 ? 2 : t->sent));
    ui_page_add(p, COL_TEXT_DIM, "xfers   %lu", (unsigned long)t->transfers);
    if (done) {
        ui_page_add(p, COL_TEXT_DIM, "result: see the GBA");
    }
}

/*
 * One owner at a time, and saying which is the whole point of the mode --
 * guessing at it is what corrupts a FAT volume.
 */
void ui_page_usb_storage(ui_page_t *p, const char *title, const char *footer,
                         bool exposed) {
    ui_page_init(p, title, footer);
    ui_page_add(p, COL_TEXT, "%s", exposed ? "drive is the PC's" : "drive is ours");
    ui_page_add(p, COL_TEXT_DIM, "%s", exposed ? "safe to copy files" : "press to hand over");
    if (exposed) {
        ui_page_add(p, COL_TEXT_DIM, "eject before back");
    }
}

/* ---- the rest ---------------------------------------------------------- */

void ui_render_progress(const ui_canvas_t *c, const char *title,
                        const char *name, const char *status, uint32_t pct,
                        const char *footer) {
    frame(c, title, footer);
    ui_text_clipped(c, TEXT_X, UI_LIST_Y + 4, TEXT_W, name ? name : "", COL_TEXT);
    ui_text_clipped(c, TEXT_X, UI_LIST_Y + 22, TEXT_W, status ? status : "",
                    COL_CURSOR);

    /* A bar, because a percentage alone does not show that a transfer has
     * stopped moving. */
    if (pct > 100) {
        pct = 100;
    }
    int bw = UI_SCREEN_W - 8;
    ui_fill(c, 4, UI_LIST_Y + 40, bw, 8, COL_BAR_TRACK);
    ui_fill(c, 4, UI_LIST_Y + 40, (int)((bw * (int)pct) / 100), 8, COL_BAR_THUMB);

    char text[12];
    snprintf(text, sizeof text, "%lu%%", (unsigned long)pct);
    ui_text_clipped(c, 4, UI_LIST_Y + 54, 60, text, COL_TEXT_DIM);
}

void ui_render_notice(const ui_canvas_t *c, const char *title, const char *text) {
    frame(c, title, NULL);
    ui_text_clipped(c, TEXT_X, UI_LIST_Y + 20, TEXT_W, text ? text : "", COL_TEXT);
}

/*
 * A black panel is indistinguishable from a dead display, a dead UART, a dead
 * task or a wrong QEMU serial mapping. Say which it is instead.
 */
void ui_render_no_interface(const ui_canvas_t *c) {
    frame(c, "NO INTERFACE", NULL);
    ui_text(c, 4, UI_LIST_Y + 6,  "shell not started", COL_TEXT);
    ui_text(c, 4, UI_LIST_Y + 20, "app_main must call", COL_TEXT_DIM);
    ui_text(c, 4, UI_LIST_Y + 32, "gate_shell_start()", COL_TEXT_DIM);
    ui_text(c, 4, UI_LIST_Y + 52, "display+uart are OK", COL_TEXT_DIM);
    ui_text(c, 4, UI_LIST_Y + 64, "if you can read this", COL_TEXT_DIM);
}
