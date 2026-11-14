/*
 * mp_wire.h -- a Multi-Player link cable between the board and one GBA, as
 * shared memory, for when the GBA is an emulator in another process
 * (test/mgba: mGBA running the real game) and the board is the simulator's
 * firmware (gblink_sim with GBLINK_MP_WIRE=<file>), as rfu_wire.h is for the
 * Wireless Adapter.
 *
 * Two units on the cable, the board and the GBA (GBATEK, SIO Multi-Player
 * Mode):
 *
 *   board MASTER (GBA-Link's SetModeMaster: the board drives the clock)
 *       the GBA is the child, unit 1. The board posts a word (req); the GBA
 *       side runs the transfer when the GBA is in Multi-Player mode:
 *       SIOMULTI0 = the board's word, SIOMULTI1 = the GBA's SIOMLT_SEND,
 *       and gives the GBA's word back (rx).
 *   board SLAVE (SetModeSlave: the GBA is the parent, unit 0)
 *       the GBA's Start runs a transfer; the board's staged word goes in
 *       SIOMULTI1 (FFFF if none is staged: nobody answered), and the GBA's
 *       word goes to the board, in order, through the ring.
 *
 * Every field under `lock`; times are CLOCK_MONOTONIC microseconds.
 */
#ifndef MP_WIRE_H
#define MP_WIRE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define MP_WIRE_MAGIC   0x5057504Du   /* "MPWP" */
#define MP_WIRE_VERSION 1
#define MP_WIRE_RING    64

enum { MP_WIRE_OFF = 0, MP_WIRE_MASTER = 1, MP_WIRE_SLAVE = 2 };

struct mp_wire {
    uint32_t magic, version;
    atomic_flag lock;

    /* ---- the board drives ---- */
    uint8_t  mode;               /* MP_WIRE_*                                 */
    bool     req_pending;        /* MASTER: a word to clock                   */
    uint16_t req_word;
    bool     staged_valid;       /* SLAVE: the answer for the next transfer   */
    uint16_t staged;

    /* ---- the GBA drives ---- */
    bool     gba_multi;          /* the GBA is in Multi-Player mode           */
    bool     rx_ready;           /* MASTER: the transfer is done              */
    uint16_t rx_word;
    uint16_t ring[MP_WIRE_RING]; /* SLAVE: the parent's words, in order       */
    uint32_t ring_head, ring_tail;

    /* counters */
    uint32_t transfers, unanswered, not_multi;
    int64_t  gba_us;
};

static inline void mp_wire_lock(struct mp_wire *w) {
    while (atomic_flag_test_and_set_explicit(&w->lock, memory_order_acquire)) { }
}
static inline void mp_wire_unlock(struct mp_wire *w) {
    atomic_flag_clear_explicit(&w->lock, memory_order_release);
}

#endif
