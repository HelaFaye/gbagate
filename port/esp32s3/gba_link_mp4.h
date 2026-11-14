/*
 * gba_link_mp4.h -- the link driver's four-player mode, on the carrier's
 * chain of up to three GBAs (doc/CARRIER.md, src/mp_bus.h). Only boards with
 * BOARD_LINK_PORTS > 1 have it.
 *
 * The client is the relay (src/mp_relay.c behind a lock): before each
 * transfer the driver asks it for the words of the board's slots, and after
 * it hands over every slot's word. Both run on the link task, on core 1.
 */
#ifndef GBA_LINK_MP4_H
#define GBA_LINK_MP4_H

#include <stdbool.h>
#include <stdint.h>

#include "mp_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* The board's slots' words for the next transfer (the others are
     * ignored). As parent, also the wait before it, in us: the remote
     * parent's cadence. */
    void (*next)(void *ctx, uint16_t tx[4], uint32_t *gap_us);
    /* Every slot's word, MPB_NO_WORD for a slot that never came. */
    void (*done)(void *ctx, const uint16_t rx[4]);
    void *ctx;
} gba_link_mp4_client_t;

/* Take the port for a four-player link in this layout; false if the layout
 * is not one the chain can carry, or the port is lent to the Wireless
 * Adapter. */
bool gba_link_mp4_start(const mp_bus_layout_t *l, const gba_link_mp4_client_t *c);
void gba_link_mp4_stop(void);
void gba_link_mp4_stats(mp_bus_stats_t *out);

/* GBAs on the chain from port 1, or -1 while the port is in use. */
int gblink_cable_gbas(void);

#ifdef __cplusplus
}
#endif

#endif
