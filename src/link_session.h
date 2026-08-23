/*
 * link_session.h -- pacing for the master role.
 *
 * Why this exists
 * ---------------
 * GBATEK, Multi-Player Mode: "The parent unit blindly sends data regardless of
 * whether childs have already processed old data/supplied new data. So, parent
 * unit might be required to insert delays between each transfer."
 *
 * That sentence is the whole reason a networked link adapter is possible. If
 * *we* are the parent, we choose when each transfer begins, and the
 * specification explicitly anticipates us inserting delays. The remote peer's
 * latency becomes a pause between transfers instead of a missed slot.
 *
 * There is one crude child-side brake, and it is worth knowing about before
 * you rely on being parent. GBATEK: "slave units may signalize that they are
 * not ready by temporarily switching into another communication mode (which
 * does not output SD High, as Multi-Player mode does during inactivity)." A
 * parent that checks SIOCNT bit 3 before starting will therefore wait. But it
 * is all-or-nothing -- to the parent it is indistinguishable from a bad cable,
 * not "hold on one frame" -- and nothing obliges a game to check it at all.
 * Treat it as an escape hatch to test per-game, never as a flow-control
 * primitive.
 *
 * So this module answers one question, once per transfer: do we have the
 * remote word yet, and if not, how much longer may we stall before the local
 * game's own timeout gives up on us?
 */

#ifndef LINK_SESSION_H
#define LINK_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "link_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How many transfers ahead the peer may run. Depth 4 lets a peer that is
 * comfortably ahead absorb a jitter spike without stalling us, and costs 4
 * transfers of input latency in the worst case. Must be a power of two.
 */
#define LS_DEPTH 4
#define LS_MASK  (LS_DEPTH - 1)

/*
 * Default stall budget. Games time out on a silent parent, but not quickly --
 * this is well inside what a frame-paced title tolerates, and comfortably
 * above a LAN round trip (USB CDC well under 1 ms, ESP-NOW 1-3 ms).
 */
#define LS_DEFAULT_STALL_US 8000u

typedef struct {
    uint16_t seq;                 /* sequence of the next transfer         */
    uint16_t word[LS_DEPTH];      /* remote words, indexed by seq & LS_MASK */
    bool     have[LS_DEPTH];
    uint32_t stall_budget_us;

    /* health counters, worth putting on the status bar */
    uint32_t transfers;
    uint32_t stalls;      /* transfers that had to wait at all          */
    uint32_t underruns;   /* transfers that gave up and sent 0xFFFF     */
    uint32_t late;        /* words that arrived for an already-run seq  */
    uint32_t dupes;       /* words for a seq we already had             */
    uint32_t peak_wait_us;
} link_session_t;

void ls_init(link_session_t *ls, uint32_t stall_budget_us);
void ls_reset(link_session_t *ls, uint16_t seq);

/*
 * Accept a word from the peer. Rejects sequences outside the window, so a
 * duplicated or reordered datagram cannot corrupt a slot that is still in use.
 */
bool ls_push_word(link_session_t *ls, uint16_t seq, uint16_t word);

/* Is the word for the current transfer already in hand? */
bool ls_ready(const link_session_t *ls);

/*
 * Claim the word for the current transfer.
 *
 * waited_us is how long the caller spun waiting for it. Pass 0 if it was
 * already there. Returns the word to put on the bus: the peer's word if it
 * arrived, or GBA_WORD_EMPTY if the budget ran out, which is exactly what the
 * hardware would show for a slot nobody drove.
 */
uint16_t ls_take_word(link_session_t *ls, uint32_t waited_us, bool *underran);

/* Advance to the next transfer. Call once per completed transfer. */
void ls_complete(link_session_t *ls);

/* How far ahead the peer currently is, 0..LS_DEPTH. */
unsigned ls_buffered(const link_session_t *ls);

#ifdef __cplusplus
}
#endif

#endif /* LINK_SESSION_H */
