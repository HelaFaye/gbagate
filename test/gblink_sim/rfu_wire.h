/*
 * rfu_wire.h -- the link cable between a Wireless Adapter and a GBA, as a
 * block of shared memory, for when the GBA is an emulator in another process
 * (test/mgba: mGBA running the real game) and the adapter is the simulator's
 * firmware (gblink_sim with GBLINK_RFU_WIRE=<file>).
 *
 * The same wires sim_rfu.c models inside the simulator, split by who drives
 * them:
 *
 *   adapter side (sim_rfu_wire.c, gbl_rfu_phy_t)   staged word, SI as clock
 *                                                  master, its role, words
 *                                                  it clocks into the GBA
 *   GBA side (test/mgba/mgba_shim.c, an mGBA SIO   SIOCNT/SIODATA32, SO, SD,
 *   driver, the emulator paced to real time)       transfers the GBA clocks
 *
 * Every field is read and written under `lock` (a spinlock: both sides hold
 * it for a few instructions). Times are CLOCK_MONOTONIC microseconds, the
 * same clock in both processes.
 */
#ifndef RFU_WIRE_H
#define RFU_WIRE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define RFU_WIRE_MAGIC   0x52465557u   /* "WUFR" */
#define RFU_WIRE_VERSION 1
#define RFU_WIRE_RING    64

struct rfu_wire {
    uint32_t magic, version;
    atomic_flag lock;

    /* ---- the adapter drives ---- */
    bool     enabled;
    bool     adapter_master;     /* RFU_ROLE_ADAPTER_MASTER: it clocks      */
    bool     staged_valid;       /* a word is staged for the GBA to clock   */
    uint32_t staged;
    bool     a_si;               /* SI, while the adapter is clock master   */

    /* the adapter clocks a word: it sets in_word and in_at, the GBA side
     * finishes the transfer at in_at (SIODATA32 = in_word, start cleared,
     * the serial IRQ) */
    bool     in_pending;
    uint32_t in_word;
    int64_t  in_at;

    /* ---- the GBA drives ---- */
    bool     gba_so;             /* its SO line                              */
    bool     armed;              /* external clock, start set, not shifting  */
    bool     busy;               /* a transfer is shifting                   */
    uint32_t gba_tx;             /* SIODATA32 as the GBA last wrote it       */
    bool     serial_owed;        /* a transfer ended; the GBA has not yet
                                    written SIOCNT again (its handler)       */
    bool     sd_level;
    int64_t  sd_rise_us;
    bool     sd_pulse_pending, sd_pulse_reported;

    /* words the GBA clocked into the adapter, for its transfer-done
     * callback, in order */
    uint32_t ring[RFU_WIRE_RING];
    uint32_t ring_head, ring_tail;

    /* counters, as sim_rfu.c's */
    uint32_t gba_words, adapter_words, unstaged, unarmed, clash, late_arm;
    int64_t  gba_us;             /* the emulator's clock, for pacing reports */
};

static inline void rfu_wire_lock(struct rfu_wire *w) {
    while (atomic_flag_test_and_set_explicit(&w->lock, memory_order_acquire)) { }
}
static inline void rfu_wire_unlock(struct rfu_wire *w) {
    atomic_flag_clear_explicit(&w->lock, memory_order_release);
}

/* The adapter's SI as the GBA reads it (SIOCNT bit 2), under the lock.
 * As upstream's PIO: with the GBA clocking, SI is low after a word until the
 * next is staged, then NOT SO; as clock master, what the adapter drives. */
static inline bool rfu_wire_adapter_si(const struct rfu_wire *w) {
    if (!w->enabled) return true;                      /* pulled up */
    if (!w->adapter_master) return w->staged_valid && !w->gba_so;
    return w->a_si;
}

#endif
