/*
 * mp_link.h -- the four-player relay (src/mp_relay.h) as the link driver's
 * client (port/esp32s3/gba_link_mp4.h), for a layout of the carrier's chain
 * (src/mp_bus.h).
 *
 * The driver calls next() before each transfer and done() after, on the
 * link task; the network side pushes the other players' commands and pops
 * the local GBAs' from its own task. The relay is behind the caller's lock
 * for both.
 *
 * As parent (a remote player is the parent and the board stands in for it)
 * the board also keeps the time: the gap before each transfer is the
 * parent's cadence as four Emeralds showed it (test/mgba/mgba_multi.c,
 * link4.py): about 16.8 ms between handshake transfers, 4.0 ms before a
 * frame's first word and 1.6 ms between the words of a frame.
 */
#ifndef MP_LINK_H
#define MP_LINK_H

#include <stdint.h>

#include "mp_bus.h"
#include "mp_relay.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_LINK_GAP_HANDSHAKE_US 16800u
#define MP_LINK_GAP_FRAME_US      4000u
#define MP_LINK_GAP_WORD_US       1600u

typedef struct {
    mp_relay_t      relay;
    mp_bus_layout_t layout;
    void          (*lock)(void *ctx);
    void          (*unlock)(void *ctx);
    void           *lock_ctx;
} mp_link_t;

/* The relay set up for the layout: its local GBAs' slots are the ones the
 * board does not answer. lock/unlock may be NULL (one task). */
void mp_link_init(mp_link_t *m, const mp_bus_layout_t *l,
                  void (*lock)(void *), void (*unlock)(void *), void *lock_ctx);

/* The driver's client calls (ctx is the mp_link_t). */
void mp_link_next(void *ctx, uint16_t tx[4], uint32_t *gap_us);
void mp_link_done(void *ctx, const uint16_t rx[4]);

/* The wait before the next transfer, as parent. */
uint32_t mp_link_gap_us(const mp_relay_t *r);

#ifdef __cplusplus
}
#endif

#endif
