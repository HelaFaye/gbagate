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

/* ---- keys and footers -------------------------------------------------- */

void ui_page_keys(ui_page_t *p, const ui_keys_t *keys, const char *verb) {
    p->footer[0] = p->footer_right[0] = '\0';
    if (!keys) {
        return;
    }
    if (verb && keys->confirm) {
        snprintf(p->footer, sizeof p->footer, "%s %s", keys->confirm, verb);
    }
    if (keys->back) {
        snprintf(p->footer_right, sizeof p->footer_right, "%s back", keys->back);
    }
}

void ui_keys_hint(char *out, size_t cap, const ui_keys_t *keys, const char *verb) {
    ui_page_t p;
    ui_page_keys(&p, keys, verb);
    snprintf(out, cap, "%s%s%s", p.footer, p.footer[0] && p.footer_right[0] ? "  " : "",
             p.footer_right);
}

#define FOOTER_GAP 6

bool ui_page_footer_fits(const ui_page_t *p) {
    uint16_t l = ui_font_width(p->footer), r = ui_font_width(p->footer_right);
    return l + r + ((l && r) ? FOOTER_GAP : 0) <= TEXT_W;
}

/* Header, background and footer: what every screen here shares. */
static void frame(const ui_canvas_t *c, const char *title, const char *footer,
                  const char *right) {
    ui_fill(c, 0, 0, UI_SCREEN_W, UI_SCREEN_H, COL_BG);
    ui_fill(c, 0, 0, UI_SCREEN_W, UI_HEADER_H, COL_HEADER);
    ui_text_clipped(c, 3, (UI_HEADER_H - UI_FONT_H) / 2, UI_SCREEN_W - 6,
                    title, COL_ON_HEADER);
    if (!footer && !right) {
        return;
    }
    int fy = UI_SCREEN_H - UI_FOOTER_H;
    int ty = fy + (UI_FOOTER_H - UI_FONT_H) / 2;
    ui_fill(c, 0, fy, UI_SCREEN_W, UI_FOOTER_H, COL_ROW_ALT);
    ui_fill(c, 0, fy, UI_SCREEN_W, 1, COL_HEADER);
    int rw = right && right[0] ? (int)ui_font_width(right) : 0;
    if (rw) {
        ui_text(c, UI_SCREEN_W - TEXT_X - rw, ty, right, COL_TEXT_DIM);
    }
    /* The left half never runs into the right one. */
    ui_text_clipped(c, TEXT_X, ty, TEXT_W - (rw ? rw + FOOTER_GAP : 0),
                    footer ? footer : "", COL_TEXT);
}

void ui_render_page(const ui_canvas_t *c, const ui_page_t *p) {
    frame(c, p->title, p->footer, p->footer_right);
    for (uint8_t i = 0; i < p->count; i++) {
        ui_text_clipped(c, TEXT_X, LINE_Y0 + i * LINE_PITCH, TEXT_W,
                        p->line[i], p->colour[i]);
    }
}

/* ---- the pages --------------------------------------------------------- */

/*
 * Every page has the same shape: what is happening, in a status colour, on
 * the first line; what to do about it next; then a fact or two. The counters
 * an engineer wants are on Diagnostics, where a player never has to read
 * them.
 */

static void begin(ui_page_t *p, const char *title, const ui_keys_t *keys,
                  const char *verb) {
    ui_page_init(p, title, NULL);
    ui_page_keys(p, keys, verb);
}

/*
 * Diagnostics is the mode you reach for when nothing else works, so it reports
 * the things that are otherwise only visible in a serial log: which
 * capabilities were detected, whether the link ever moved, and whether the UI
 * is losing events.
 */
void ui_page_diagnostics(ui_page_t *p, const char *title, const ui_keys_t *keys,
                         const ui_diag_t *d) {
    char caps[64];
    hw_caps_describe(d->caps, caps, sizeof caps);
    begin(p, title, keys, NULL);
    ui_page_add(p, COL_TEXT_DIM, "detected:");
    ui_page_add_wrapped(p, COL_TEXT, caps);
    ui_page_add(p, d->linked ? COL_GOOD : COL_WARN, "link   %s",
                d->linked ? "linked" : "waiting");
    ui_page_add(p, COL_TEXT, "xfers  %lu", (unsigned long)d->transfers);
    ui_page_add(p, COL_TEXT, "chips  %lu", (unsigned long)d->chips_sent);
    ui_page_add(p, COL_TEXT, "gate   %04X", d->gate_id);
    ui_page_add(p, d->ui_dropped ? COL_WARN : COL_TEXT, "uidrop %lu",
                (unsigned long)d->ui_dropped);
}

/* Values of app_dispatch_status_t (app_modes.h), which this file need not
 * include to read. */
enum { ST_OK, ST_NO_HANDLER, ST_MISSING_CAPS, ST_NEEDS_GAME, ST_BUSY, ST_REFUSED };

void ui_page_refused(ui_page_t *p, const char *title, const ui_keys_t *keys,
                     int status) {
    begin(p, title, keys, NULL);
    ui_page_add(p, COL_BAD, "did not start");
    switch (status) {
    case ST_MISSING_CAPS:
        ui_page_add(p, COL_TEXT_DIM, "needs hardware this");
        ui_page_add(p, COL_TEXT_DIM, "board does not have");
        break;
    case ST_NEEDS_GAME:
        ui_page_add(p, COL_TEXT_DIM, "choose a game first");
        break;
    case ST_BUSY:
        ui_page_add(p, COL_TEXT_DIM, "another mode is on");
        break;
    case ST_REFUSED:
        /* The mode's own enter() said no: usually something it needs --
         * a database, a radio, the USB port -- is not there. */
        ui_page_add(p, COL_TEXT_DIM, "something it needs");
        ui_page_add(p, COL_TEXT_DIM, "is not available");
        break;
    default:
        ui_page_add(p, COL_TEXT_DIM, "not in this firmware");
        break;
    }
    ui_page_add(p, COL_TEXT_DIM, "Diagnostics may say");
}

void ui_page_status(ui_page_t *p, const char *title, const ui_keys_t *keys,
                    const char *status) {
    begin(p, title, keys, NULL);
    ui_page_add(p, COL_TEXT, "%s", status ? status : "");
}

/* How usable a measured round trip is. Gen 3 games tolerate a few
 * milliseconds; tens of them and the link drops. */
static const char *delay_word(uint32_t us, uint16_t *colour) {
    if (us < 2000)  { *colour = COL_GOOD; return "good"; }
    if (us < 8000)  { *colour = COL_WARN; return "ok"; }
    *colour = COL_BAD;
    return "slow";
}

void ui_page_link_usb(ui_page_t *p, const char *title, const ui_keys_t *keys,
                      const link_bridge_t *b) {
    begin(p, title, keys, NULL);
    if (!b) {
        ui_page_add(p, COL_BAD, "not available");
        ui_page_add(p, COL_TEXT_DIM, "this image has no");
        ui_page_add(p, COL_TEXT_DIM, "USB link");
        return;
    }
    switch (b->state) {
    case BRIDGE_READY: {
        ui_page_add(p, COL_GOOD, "connected to PC");
        if (b->rtt_us) {
            uint16_t c;
            const char *w = delay_word(b->rtt_us, &c);
            ui_page_add(p, c, "delay %lu.%lu ms %s", (unsigned long)(b->rtt_us / 1000),
                        (unsigned long)(b->rtt_us % 1000 / 100), w);
        } else {
            ui_page_add(p, COL_TEXT_DIM, "measuring delay");
        }
        ui_page_add(p, COL_TEXT_DIM, "out %lu in %lu", (unsigned long)b->words_sent,
                    (unsigned long)b->words_received);
        break;
    }
    case BRIDGE_HELLO:
        ui_page_add(p, COL_WARN, "PC found");
        ui_page_add(p, COL_TEXT_DIM, "pairing...");
        break;
    default:
        ui_page_add(p, COL_WARN, "waiting for the PC");
        ui_page_add(p, COL_TEXT_DIM, "plug in USB, then");
        ui_page_add(p, COL_TEXT_DIM, "start mGBA's link");
        break;
    }
}

const char *ui_gblink_mode_name(int mode) {
    switch (mode) {
    case 0x00: return "trade emulator";
    case 0x01: return "GBA link";
    case 0x02: return "Game Boy link";
    case 0x03: return "Game Boy Printer";
    case 0x04: return "Advance Wars";
    case 0x05: return "e-Reader";
    case 0x06: return "Battle Chip Gate";
    case 0x07: return "Wireless Adapter";
    default:   return mode >= 0x80 ? "gbagate mode" : "unknown mode";
    }
}

const char *ui_gblink_status_name(uint16_t status) {
    switch (status) {
    case 0xFF00: return "GBA connected";
    case 0xFF01: return "GBA unplugged";
    case 0xFF02: return "waiting for game";
    case 0xFF03: return "handshake";
    case 0xFF04: return "handshake done";
    case 0xFF05: return "linked";
    case 0xFF06: return "reconnecting";
    case 0xFF07: return "link closed";
    case 0xFF08: return "ready";
    case 0xFF09: return "trade finished";
    case 0xFF0A: return "Game Boy mode on";
    case 0xFF0B: return "printer ready";
    case 0xFF0C: return "session over";
    default:     return NULL;
    }
}

/*
 * The board as a GB-Link adapter: whether a PC has it, and what the PC's app
 * is running. It used to show the word bridge, which nothing binds on
 * hardware: the page claimed "PC found, pairing" with no PC attached.
 */
void ui_page_gblink(ui_page_t *p, const char *title, const ui_keys_t *keys,
                    const ui_gblink_t *g) {
    begin(p, title, keys, NULL);
    if (!g) {
        ui_page_add(p, COL_BAD, "not available");
        ui_page_add(p, COL_TEXT_DIM, "this image has no");
        ui_page_add(p, COL_TEXT_DIM, "USB link");
        return;
    }
    if (!g->usb_is_gblink) {
        ui_page_add(p, COL_WARN, "USB is the drive");
        ui_page_add_wrapped(p, COL_TEXT_DIM,
                            "set usb to GB-Link in Settings, then restart");
        return;
    }
    if (g->mode >= 0 && g->owner == UI_HOST_LOCAL) {
        /* the wireless cable or online play drives it; a PC is refused */
        ui_page_add(p, COL_WARN, "in use by a link");
        ui_page_add(p, COL_TEXT_DIM, "%s", ui_gblink_mode_name(g->mode));
        return;
    }
    if (g->mode < 0) {
        if (!g->configured && g->owner != UI_HOST_BRIDGE && g->owner != UI_HOST_NET) {
            ui_page_add(p, COL_WARN, "waiting for the PC");
            ui_page_add(p, COL_TEXT_DIM, "plug USB into a PC");
            ui_page_add(p, COL_TEXT_DIM, "or phone, then open");
            ui_page_add(p, COL_TEXT_DIM, "a GB-Link app");
        } else if (!g->host_seen) {
            ui_page_add(p, COL_GOOD, "connected to PC");
            ui_page_add(p, COL_TEXT_DIM, "open a GB-Link app");
        } else {
            ui_page_add(p, COL_GOOD, "app connected");
            ui_page_add(p, COL_TEXT_DIM, "no mode running");
        }
        return;
    }
    static const char *const via[] = { "via WebUSB", "via serial",
                                       "via companion", "", "via WiFi" };
    ui_page_add(p, COL_GOOD, "%s", ui_gblink_mode_name(g->mode));
    ui_page_add(p, COL_TEXT_DIM, "%s",
                g->owner < sizeof via / sizeof via[0] ? via[g->owner] : "");
    const char *st = ui_gblink_status_name(g->status);
    if (st) {
        uint16_t c = g->status == 0xFF05 ? COL_GOOD
                   : (g->status == 0xFF01 || g->status == 0xFF06) ? COL_WARN
                   : COL_TEXT;
        ui_page_add(p, c, "%s", st);
    }
    if (!g->configured && g->owner == UI_HOST_USB) {
        ui_page_add(p, COL_BAD, "USB unplugged");
    }
}

/* The setup page's address: ESP-IDF's SoftAP default. */
#define UI_SETUP_URL "192.168.4.1"

static void setup_lines(ui_page_t *p, const char *hotspot) {
    ui_page_add(p, COL_TEXT_DIM, "join the hotspot");
    ui_page_add(p, COL_TEXT, "%s", hotspot ? hotspot : "gbagate");
    ui_page_add(p, COL_TEXT_DIM, "open " UI_SETUP_URL);
}

/*
 * Online play: WiFi first, then Celio. Each state says what to do next,
 * because the person holding the board is the one who has to do it --
 * join a hotspot, read a code out to a friend.
 */
void ui_page_online(ui_page_t *p, const char *title, const ui_keys_t *keys,
                    const ui_online_t *o) {
    /* celio_state_t, without including celio.h in every screen's build */
    enum { C_IDLE, C_CONNECTING, C_ENTERING, C_WAITING, C_STARTING, C_LINKED, C_CLOSED, C_FAILED };
    bool again = o && o->net == UI_NET_UP && (o->celio == C_FAILED || o->celio == C_CLOSED);
    begin(p, title, keys, again ? "retry" : NULL);
    if (!o || o->net == UI_NET_NONE) {
        ui_page_add(p, COL_BAD, "not available");
        ui_page_add(p, COL_TEXT_DIM, "this image has no");
        ui_page_add(p, COL_TEXT_DIM, "WiFi");
        return;
    }
    switch (o->net) {
    case UI_NET_SETUP:
        ui_page_add(p, COL_WARN, "set up WiFi");
        setup_lines(p, o->hotspot);
        ui_page_add(p, COL_TEXT_DIM, "or put wifi.txt on");
        ui_page_add(p, COL_TEXT_DIM, "the USB drive");
        return;
    case UI_NET_JOINING:
        ui_page_add(p, COL_WARN, "joining WiFi");
        ui_page_add(p, COL_TEXT, "%s", o->ssid ? o->ssid : "");
        return;
    case UI_NET_FAILED:
        ui_page_add(p, COL_BAD, "can't join WiFi");
        ui_page_add(p, COL_TEXT, "%s", o->ssid ? o->ssid : "");
        setup_lines(p, o->hotspot);
        return;
    default:
        break;
    }
    switch (o->celio) {
    case C_IDLE:
    case C_CONNECTING:
    case C_ENTERING:
        ui_page_add(p, COL_WARN, "finding Celio...");
        break;
    case C_WAITING:
        ui_page_add(p, COL_WARN, "waiting for partner");
        ui_page_add(p, COL_TEXT_DIM, "their code to join:");
        if (o->session) {
            /* Celio's ids are 21 characters, one more than a line: split
             * them where a reader can keep their place */
            size_t n = strlen(o->session), half = (n + 1) / 2;
            ui_page_add(p, COL_TEXT, "%.*s", (int)half, o->session);
            ui_page_add(p, COL_TEXT, "%s", o->session + half);
        }
        break;
    case C_STARTING:
        ui_page_add(p, COL_GOOD, "partner joined");
        ui_page_add(p, COL_TEXT_DIM, "starting the link");
        break;
    case C_LINKED: {
        ui_page_add(p, COL_GOOD, "playing online");
        const char *st = ui_gblink_status_name(o->link_status);
        if (st) {
            ui_page_add(p, o->link_status == 0xFF06 ? COL_WARN : COL_TEXT, "%s", st);
        }
        break;
    }
    case C_CLOSED:
        ui_page_add(p, COL_TEXT, "session over");
        break;
    default:
        ui_page_add(p, COL_BAD, "online failed");
        ui_page_add_wrapped(p, COL_TEXT_DIM, o->error && *o->error ? o->error : "no reason given");
        break;
    }
    if (o->ip) {
        ui_page_add(p, COL_TEXT_DIM, "IP %s", o->ip);
    }
}

/*
 * The wireless link cable: GB-Link's relay, run against another board over
 * ESP-NOW (src/gbl_peer.c). Retransmits climbing while frames still flow is a
 * noisy channel; frames stopping is the peer gone.
 */
void ui_page_wireless(ui_page_t *p, const char *title, const ui_keys_t *keys,
                      const gbl_peer_t *peer, const uint8_t *mac,
                      const char *air_link) {
    begin(p, title, keys, NULL);
    const char *carry = air_link ? air_link : "";
    switch (peer->state) {
    case GBL_PEER_LINKED: {
        ui_page_add(p, COL_GOOD, "linked to partner");
        ui_page_add(p, COL_TEXT_DIM, "as %s", carry);
        /* Signal: the share of datagrams sent again. A few percent is a
         * normal room; a fifth is a wall or a microwave in the way. */
        if (peer->tx_datagrams >= 20) {
            uint32_t pct = (uint32_t)((peer->retransmits * 100u) / peer->tx_datagrams);
            uint16_t c = pct < 5 ? COL_GOOD : pct < 20 ? COL_WARN : COL_BAD;
            ui_page_add(p, c, "signal %s", pct < 5 ? "good" : pct < 20 ? "fair" : "poor");
        }
        break;
    }
    case GBL_PEER_SETTLING:
    case GBL_PEER_LINKING:
        ui_page_add(p, COL_WARN, "partner found");
        ui_page_add(p, COL_TEXT_DIM, "starting the link");
        break;
    case GBL_PEER_CLOSED:
        ui_page_add(p, COL_WARN, "session ended");
        ui_page_add(p, COL_TEXT_DIM, "restarting...");
        break;
    case GBL_PEER_SEARCHING:
        ui_page_add(p, COL_WARN, "looking for partner");
        if (peer->mode_mismatch) {
            /* A board is there but set to another air link. */
            ui_page_add(p, COL_TEXT_DIM, "one is set to a");
            ui_page_add(p, COL_TEXT_DIM, "different air link");
        } else {
            ui_page_add(p, COL_TEXT_DIM, "on the other one:");
            ui_page_add(p, COL_TEXT_DIM, "Link > Wireless");
        }
        ui_page_add(p, COL_TEXT_DIM, "as %s", carry);
        break;
    default:
        ui_page_add(p, COL_TEXT_DIM, "starting...");
        break;
    }

    /* Only while there is a session: the telemetry is the last session's,
     * and after it ends it describes a GBA that is no longer linked. */
    if (peer->mode == 0x07 &&
        (peer->state == GBL_PEER_LINKING || peer->state == GBL_PEER_LINKED)) {
        /* Wireless Adapter: whether the game has found the adapter is the
         * first question; the firmware's own telemetry answers it. */
        const char *gba = !peer->rfu.seen      ? "GBA: not seen yet"
                        : peer->rfu.found      ? "GBA: found adapter"
                        : !peer->rfu.transfers ? "GBA: no clock"
                        : !peer->rfu.any_rx    ? "GBA: use GBC cable"
                                               : "GBA: looking";
        ui_page_add(p, peer->rfu.found ? COL_GOOD : COL_TEXT_DIM, "%s", gba);
    }
    if (mac) {
        /* The end of the partner's address: enough to tell two boards apart
         * across a room, which is the only use it has here. */
        ui_page_add(p, COL_TEXT_DIM, "partner ..%02X%02X", mac[4], mac[5]);
    }
}

void ui_page_infrared(ui_page_t *p, const char *title, const ui_keys_t *keys,
                      const ui_ir_t *ir) {
    if (!ir->ready) {
        begin(p, title, keys, NULL);
        ui_page_add(p, COL_BAD, "no IR LED fitted");
        ui_page_add(p, COL_TEXT_DIM, "see BRINGUP for the");
        ui_page_add(p, COL_TEXT_DIM, "IR transmitter");
        return;
    }
    /* Verbs stay at five letters: "click" plus one has to share the
     * footer with "hold back". */
    begin(p, title, keys, ir->commands ? "send" : NULL);
    ui_page_add(p, COL_GOOD, "IR ready");
    if (!ir->commands) {
        ui_page_add(p, COL_TEXT_DIM, "point the Zoid at it");
        return;
    }
    ui_page_add(p, COL_TEXT_DIM, "heard %lu", (unsigned long)ir->commands);
    ui_page_add(p, COL_TEXT_DIM, "last %03X %X/%X/%X", (unsigned)ir->last,
                cdz_category(ir->last), cdz_motion(ir->last), cdz_action(ir->last));
    /*
     * Aborted trains is the number that tells you whether the timing is right.
     * A decoder measuring the wrong thing aborts nearly everything while still
     * looking alive, so a bare command count would not show it.
     */
    if (ir->aborted) {
        ui_page_add(p, COL_WARN, "garbled %lu", (unsigned long)ir->aborted);
    }
}

void ui_page_chip_scan(ui_page_t *p, const char *title, const ui_keys_t *keys,
                       const ui_scan_t *s) {
    bool readable = s->reader && (s->advance || s->beast) && s->id != CG_CHIP_NONE;
    begin(p, title, keys, readable ? "send" : NULL);
    if (!s->reader) {
        ui_page_add(p, COL_BAD, "no reader board");
        ui_page_add(p, COL_TEXT_DIM, "check its connector");
        return;
    }
    if (!s->advance && !s->beast) {
        ui_page_add(p, COL_WARN, "insert a chip");
        ui_page_add(p, COL_TEXT_DIM, "Advance or Beast");
        return;
    }
    const char *slot = s->advance ? "Advance slot" : "Beast slot";
    if (!readable) {
        ui_page_add(p, COL_WARN, "can't read the chip");
        ui_page_add(p, COL_TEXT_DIM, "reseat it in the");
        ui_page_add(p, COL_TEXT_DIM, "%s", slot);
        return;
    }
    ui_page_add(p, COL_GOOD, "%s", s->name && s->name[0] ? s->name : "chip read");
    ui_page_add(p, COL_TEXT_DIM, "id %04X, %s", s->id, s->advance ? "Advance" : "Beast");
}

void ui_page_selftest(ui_page_t *p, const char *title, const ui_keys_t *keys,
                      const ui_selftest_t *t) {
    begin(p, title, keys, NULL);
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
void ui_page_usb_storage(ui_page_t *p, const char *title, const ui_keys_t *keys,
                         bool exposed) {
    begin(p, title, keys, exposed ? "claim" : "lend");
    if (exposed) {
        ui_page_add(p, COL_WARN, "drive is on the PC");
        ui_page_add(p, COL_TEXT_DIM, "copy your files,");
        ui_page_add(p, COL_TEXT_DIM, "eject it on the PC,");
        ui_page_add(p, COL_TEXT_DIM, "then claim it back");
    } else {
        ui_page_add(p, COL_TEXT, "drive is gbagate's");
        ui_page_add(p, COL_TEXT_DIM, "lend it to the PC to");
        ui_page_add(p, COL_TEXT_DIM, "copy programs on");
    }
}

const char *ui_send_result_text(int r, const char **advice) {
    const char *a = "";
    const char *t;
    /* gba_multiboot.h's mb_result_t. */
    switch (r) {
    case 0:  t = "sent";                a = "the GBA runs it now";  break;
    case 2:  t = "no GBA answered";     a = "power it on, no cart"; break;
    case 3:  t = "GBA didn't agree";    a = "restart it and retry"; break;
    case 4:  t = "transfer stopped";    a = "check the cable";      break;
    case 5:  t = "arrived damaged";     a = "retry; check cable";   break;
    case 1:  t = "can't send this";     a = "the file is unusable"; break;
    default: t = "failed";              break;
    }
    if (advice) {
        *advice = a;
    }
    return t;
}

/* ---- the rest ---------------------------------------------------------- */

void ui_render_progress(const ui_canvas_t *c, const char *title,
                        const char *name, const char *status,
                        const char *advice, uint32_t pct, const char *footer) {
    frame(c, title, footer, NULL);
    ui_text_clipped(c, TEXT_X, UI_LIST_Y + 4, TEXT_W, name ? name : "", COL_TEXT_DIM);
    ui_text_clipped(c, TEXT_X, UI_LIST_Y + 20, TEXT_W, status ? status : "",
                    COL_CURSOR);
    ui_text_clipped(c, TEXT_X, UI_LIST_Y + 31, TEXT_W, advice ? advice : "",
                    COL_TEXT);

    /* A bar, because a percentage alone does not show that a transfer has
     * stopped moving. */
    if (pct > 100) {
        pct = 100;
    }
    int bw = UI_SCREEN_W - 8;
    ui_fill(c, 4, UI_LIST_Y + 48, bw, 8, COL_BAR_TRACK);
    ui_fill(c, 4, UI_LIST_Y + 48, (int)((bw * (int)pct) / 100), 8, COL_BAR_THUMB);

    char text[12];
    snprintf(text, sizeof text, "%lu%%", (unsigned long)pct);
    ui_text_clipped(c, 4, UI_LIST_Y + 62, 60, text, COL_TEXT_DIM);
}

void ui_render_notice(const ui_canvas_t *c, const char *title, const char *text) {
    frame(c, title, NULL, NULL);
    /* Wrapped, so a notice can say what to do as well as what is wrong. */
    ui_page_t p;
    ui_page_init(&p, NULL, NULL);
    ui_page_add_wrapped(&p, COL_TEXT, text ? text : "");
    for (uint8_t i = 0; i < p.count; i++) {
        ui_text_clipped(c, TEXT_X, UI_LIST_Y + 20 + i * LINE_PITCH, TEXT_W,
                        p.line[i], i ? COL_TEXT_DIM : COL_TEXT);
    }
}

/*
 * A black panel is indistinguishable from a dead display, a dead UART, a dead
 * task or a wrong QEMU serial mapping. Say which it is instead.
 */
void ui_render_no_interface(const ui_canvas_t *c) {
    frame(c, "NO INTERFACE", NULL, NULL);
    ui_text(c, 4, UI_LIST_Y + 6,  "shell not started", COL_TEXT);
    ui_text(c, 4, UI_LIST_Y + 20, "app_main must call", COL_TEXT_DIM);
    ui_text(c, 4, UI_LIST_Y + 32, "gate_shell_start()", COL_TEXT_DIM);
    ui_text(c, 4, UI_LIST_Y + 52, "display+uart are OK", COL_TEXT_DIM);
    ui_text(c, 4, UI_LIST_Y + 64, "if you can read this", COL_TEXT_DIM);
}
