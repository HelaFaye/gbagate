/*
 * sim_main.c -- the GB-Link personality of the gbagate firmware as a host
 * process. See sim_port.c for the record format on stdin/stdout.
 *
 *     gblink_sim [flash-file]
 *
 * With a flash file, settings persist across runs the way they persist across
 * power cycles on the board.
 */
#include <sys/random.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#include "gblink_port.h"
#include "gbprinter.h"
#include "sim.h"
#include "sim_rfu.h"
#include "zshim.h"

static bool read_full(uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(0, buf + got, n - got);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

/* The radio, for the wireless link cable: datagrams go out as 'P' records and
 * come in the same way, so a test can join two simulators and decide what the
 * air between them loses. */
bool gblink_peer_radio_send(const uint8_t *d, size_t n) {
    sim_emit('P', 0, d, n);
    return true;
}

/* The Celio client's WebSocket: frames go out as 'W' records (ep 0 text,
 * ep 1 [on] for connect/disconnect), and come in the same way (ep 0 text,
 * ep 1 opened, ep 2 closed), so a test can carry them to a real server. */
bool gblink_celio_ws_send(const char *t, size_t n) {
    sim_emit('W', 0, (const uint8_t *)t, n);
    return true;
}

void gblink_celio_ws_connect(bool on) {
    uint8_t b = on;
    sim_emit('W', 1, &b, 1);
}

/* From the kernel's entropy, as the board uses its hardware RNG. Two
 * simulators sharing a Celio clientId would have the server take the second
 * for a reconnect of the first and drop the first's socket. A seed of
 * getpid() ^ time(NULL) did exactly that in CI: simulators started back to
 * back have neighbouring PIDs, and pids p and p ^ 1 started in seconds t and
 * t ^ 1 seed alike. */
uint32_t gblink_celio_random(void) {
    uint32_t v;
    if (getrandom(&v, sizeof v, 0) == (ssize_t)sizeof v) return v;
    static uint64_t x;                       /* fallback: splitmix64 */
    if (!x) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        x = ((uint64_t)getpid() << 32) ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 20);
    }
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)(z ^ (z >> 31));
}

void gblink_peer_observe_status(uint16_t s) {
    uint8_t b[2] = { (uint8_t)s, (uint8_t)(s >> 8) };
    sim_event(SIM_EV_PEER_STATUS, b, 2);
}

int main(int argc, char **argv) {
    /* gblink_sim [--peer NONCE[:MODE[:VARIANT]]] [flash-file] */
    uint32_t peer_nonce = 0;
    unsigned peer_mode = 1, peer_variant = 0;
    int argi = 1;
    if (argi + 1 < argc + 1 && argi < argc && strcmp(argv[argi], "--peer") == 0 && argi + 1 < argc) {
        sscanf(argv[argi + 1], "%u:%u:%u", &peer_nonce, &peer_mode, &peer_variant);
        argi += 2;
    }
    sim_storage_init(argi < argc ? argv[argi] : NULL);
    sim_gba_init();
    sim_rfu_init();
    sim_rfu_game_init();
    gblink_start();

    /* The board enumerates once the stack is up; a host test can tell it to
     * drop off the bus with a zero-length 'U' record to ep 0xFF. */
    gblink_usb_on_configured(true);
    if (peer_nonce) gblink_peer_start((uint8_t)peer_mode, (uint8_t)peer_variant, peer_nonce);
    sim_event(SIM_EV_READY, NULL, 0);

    uint8_t hdr[4];
    static uint8_t buf[65536];
    while (read_full(hdr, 4)) {
        size_t len = (size_t)hdr[2] | ((size_t)hdr[3] << 8);
        if (len && !read_full(buf, len)) break;
        switch (hdr[0]) {
        case 'U':
            if (hdr[1] == 0xFF) gblink_usb_on_configured(len != 0);
            else gblink_usb_on_receive(hdr[1], buf, len);
            break;
        case 'C': gblink_cdc_on_receive(buf, len); break;
        case 'B': gblink_bridge_on_receive(buf, len); break;
        case 'N': gblink_net_on_receive(buf, len); break;
        case 'G': sim_gba_control(buf, len); break;
        case 'P': gblink_peer_radio_rx(buf, len); break;
        case 'S':
            /* start the wireless cable now (or with --peer, at boot) */
            if (len >= 4) {
                uint32_t n = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                             ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
                gblink_peer_start(len >= 5 ? buf[4] : 1, len >= 6 ? buf[5] : 0, n);
            } else {
                gblink_peer_stop();
            }
            break;
        case 'W':
            if (hdr[1] == 0) gblink_celio_ws_text((const char *)buf, len);
            else if (hdr[1] == 1) gblink_celio_ws_open();
            else gblink_celio_ws_closed();
            break;
        case 'c':
            /* start the Celio client: the body is the session to join, or
             * empty to create one; ep 1 stops it */
            if (hdr[1] == 1) {
                gblink_celio_stop();
            } else if (hdr[1] == 2 && len >= 2) {
                /* a Multi-Play session: [players, gbas, session...] */
                char sid[40] = "";
                if (len - 2 < sizeof sid) { memcpy(sid, buf + 2, len - 2); sid[len - 2] = '\0'; }
                gblink_celio_start_multi(sid, buf[0], buf[1]);
            } else {
                char sid[40] = "";
                if (len < sizeof sid) { memcpy(sid, buf, len); sid[len] = '\0'; }
                gblink_celio_start(sid);
            }
            break;
        case 'p': {
            /* the board's printer (printer_glue.cpp): ep 0 start, 1 stop,
             * 2 drain what it sent into the decoder the UI uses and report */
            static gbp_t pr;
            if (hdr[1] == 0) { gbp_init(&pr); gblink_printer_start(); }
            else if (hdr[1] == 1) gblink_printer_stop();
            else {
                uint8_t chunk[256];
                size_t got;
                while ((got = gblink_printer_read(chunk, sizeof chunk)) > 0) gbp_feed_stream(&pr, chunk, got);
                gblink_printer_view_t v;
                gblink_printer_view(&v);
                uint32_t sum = 0;
                for (int r = 0; r < gbp_rows(&pr); r++)
                    for (int x = 0; x < GBP_WIDTH; x++) sum += gbp_shade(&pr, r, x);
                const uint32_t f[5] = { pr.packets, (uint32_t)gbp_rows(&pr), pr.pictures, pr.aborts, v.lost };
                uint8_t b[sizeof f + 2 + 4];
                memcpy(b, f, sizeof f);
                b[20] = (uint8_t)v.status; b[21] = (uint8_t)(v.status >> 8);
                memcpy(b + 22, &sum, 4);
                sim_event(SIM_EV_PRINTER, b, sizeof b);
            }
            break;
        }
        case 'm': sim_mp4_report(); break;
        case 'K': {
            gblink_celio_view_t v;
            gblink_celio_view(&v);
            uint8_t b[3 + 28 + sizeof v.session + sizeof v.error];
            size_t o = 0;
            b[o++] = (uint8_t)v.state;
            b[o++] = (uint8_t)v.link_status;
            b[o++] = (uint8_t)(v.link_status >> 8);
            memcpy(b + o, &v.up, 4); o += 4;
            memcpy(b + o, &v.down, 4); o += 4;
            memcpy(b + o, &v.lost, 4); o += 4;
            memcpy(b + o, &v.reconnects, 4); o += 4;
            memcpy(b + o, &v.resent, 4); o += 4;
            memcpy(b + o, &v.given_up, 4); o += 4;
            memcpy(b + o, &v.max_rtt_ms, 4); o += 4;
            size_t sl = strnlen(v.session, sizeof v.session - 1);
            memcpy(b + o, v.session, sl); o += sl; b[o++] = 0;
            size_t el = strnlen(v.error, sizeof v.error - 1);
            memcpy(b + o, v.error, el); o += el; b[o++] = 0;
            sim_event(SIM_EV_CELIO, b, o);
            break;
        }
        case 'I': {
            gblink_state_t st;
            gblink_state(&st);
            uint8_t b[5 + 4 + 4 * GBLINK_HOSTS];
            b[0] = st.usb_configured;
            b[1] = (uint8_t)(int8_t)st.mode;
            b[2] = st.owner;
            b[3] = (uint8_t)st.last_status;
            b[4] = (uint8_t)(st.last_status >> 8);
            memcpy(b + 5, &st.statuses, 4);
            memcpy(b + 9, st.frames, sizeof st.frames);
            sim_event(SIM_EV_STATE, b, sizeof b);
            break;
        }
        case 'Q': exit(0);
        default: break;
        }
    }
    return 0;
}
