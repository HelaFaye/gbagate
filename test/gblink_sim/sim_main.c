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

#include "gblink_port.h"
#include "sim.h"
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

int main(int argc, char **argv) {
    sim_storage_init(argc > 1 ? argv[1] : NULL);
    sim_gba_init();
    gblink_start();

    /* The board enumerates once the stack is up; a host test can tell it to
     * drop off the bus with a zero-length 'U' record to ep 0xFF. */
    gblink_usb_on_configured(true);
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
        case 'Q': exit(0);
        default: break;
        }
    }
    return 0;
}
