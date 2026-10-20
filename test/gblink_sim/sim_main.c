/*
 * sim_main.c -- the GB-Link personality of the gbagate firmware as a host
 * process. See sim_port.c for the record format on stdin/stdout.
 *
 *     gblink_sim [flash-file]
 *
 * With a flash file, settings persist across runs the way they persist across
 * power cycles on the board.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string.h>

#include "gblink_port.h"
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
        case 'Q': exit(0);
        default: break;
        }
    }
    return 0;
}
