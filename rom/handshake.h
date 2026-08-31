/*
 * handshake.h -- the game side of the Battle Chip Gate handshake, as a pure
 * function of the word observed from the gate each frame.
 *
 * This is the deliberate answer to the "honest caveat" in qemu/README.md. The
 * retail path drives a real game's SIOCNT from Lua and depends on how closely
 * that game watches the start/busy bits -- the least certain part of the setup.
 * A purpose-built tester ROM has no such dependency: it never touches SIOCNT.
 * Each frame it reads the gate's word from SIOMULTI0 and stages its own into
 * SIOMLT_SEND, and this header decides what to stage. The logic is edge-driven
 * on the *observed* word rather than on frame timing, so it tolerates the frame
 * or two of latency that a QEMU firmware <-> broker <-> mGBA round trip adds.
 *
 * It has no GBA or ESP dependencies on purpose, so the same object code runs in
 * the ROM (rom/tester.c) and in a host self-test (qemu/test_rom_selftest.py)
 * that drives it against the real src/chipgate.c. Getting a green self-test on
 * the host is exactly the "prove the driver before the emulator" step.
 *
 * The sequence mirrors broker.run_gate() / qemu/test_gate_drive.py: wait for the
 * gate to announce its identity, send the sync word, send the acknowledge, then
 * idle and watch the 9-slot rotation come round.
 */

#ifndef ROM_HANDSHAKE_H
#define ROM_HANDSHAKE_H

#include <stdbool.h>
#include <stdint.h>

/* Wire constants, mirroring src/chipgate.h. Duplicated rather than included so
 * the ROM needs none of the firmware's headers. */
#define HS_WORD_SYNC     0x8FFFu /* game -> gate: are you there?        */
#define HS_WORD_CONTINUE 0xFFFFu /* gate filler / "present, nothing new" */
#define HS_WORD_IDLE     0x0000u /* game idle word                      */

/* Default target: MMBN6 US Beast Link Gate, which is what the QEMU firmware
 * announces (tasks.c opens the "bn6g" database on boot). */
#define HS_GATE_ID_BN6US 0xFF00u

/* Enough LINKED frames to have watched a couple of full 9-slot rotations. */
#define HS_LINK_EVIDENCE_FRAMES 24

/*
 * The rotation is nine words and slot 0 is the gate identity, which is the one
 * value that cannot be confused with any other slot (0x0000 and 0xFFFF both
 * appear elsewhere). So seeing the identity is what anchors the index, and
 * every other slot is counted from there. Without that anchor a watcher can
 * only say "these nine words went past", which is what the first version of
 * this header did -- enough to prove a link, not enough to prove a chip.
 */
#define HS_ROT_LEN      9
#define HS_SLOT_ID      0   /* gate identity  */
#define HS_SLOT_FILL1   1   /* 0xFFFF         */
#define HS_SLOT_FILL2   2   /* 0xFFFF         */
#define HS_SLOT_DATA0   3   /* rolling counter, steps +3 (8-bit)      */
#define HS_SLOT_DATA1   4   /* rolling counter, steps down            */
#define HS_SLOT_CHIP    5   /* the slotted chip, or 0x0000 when empty */
#define HS_SLOT_END0    6   /* 0x0000         */
#define HS_SLOT_END1    7
#define HS_SLOT_END2    8

/* Nine rotations, matching CG_CHIP_REPEATS in src/chipgate.h. */
#define HS_CHIP_REPEATS 9

/*
 * The self-test chip is chosen so it cannot be confused with either rolling
 * counter, which matters because a watcher can only identify slots by
 * position and position can drift:
 *
 *   DATA0 is 8-bit  -- it takes every value 0x00..0xFF, so any chip <= 0xFF
 *                      will collide with it sooner or later. Chip 1 (Cannon)
 *                      does, which is how this was found: the counter reaching
 *                      0x0001 looked like a chip arriving.
 *   DATA1 steps in  -- its low ten bits are always zero, so any value that is
 *   0x400 units       not a multiple of 0x400 is safe.
 *
 * 0x0101 satisfies both and is a real chip in every database.
 */
#define HS_SELFTEST_CHIP 0x0101u

/* Individual checks, reported as a bitmask so a partial pass is legible. */
#define HS_T_LINK      (1u << 0)  /* reached LINKED, identity as expected   */
#define HS_T_SHAPE     (1u << 1)  /* fixed slots hold their fixed values    */
#define HS_T_COUNTERS  (1u << 2)  /* the two data slots actually advance    */
#define HS_T_CHIP      (1u << 3)  /* a chip arrived in slot 5               */
#define HS_T_REPEATS   (1u << 4)  /* held for 9 rotations, then cleared     */
#define HS_T_RELINK    (1u << 5)  /* teardown and a second handshake        */

#define HS_T_ALL (HS_T_LINK | HS_T_SHAPE | HS_T_COUNTERS | \
                  HS_T_CHIP | HS_T_REPEATS | HS_T_RELINK)

/* Tests that need the firmware to do something. Reported separately so a run
 * against a gate that never sends a chip is legible rather than just "failed". */
#define HS_T_NEEDS_FIRMWARE (HS_T_CHIP | HS_T_REPEATS)

typedef enum {
    HS_IDLE = 0,   /* waiting to see the gate announce its identity        */
    HS_ANNOUNCED,  /* saw the identity; about to send the sync word        */
    HS_SYNCED,     /* sent sync; about to send the acknowledge             */
    HS_LINKED      /* sent the acknowledge; session should be rotating     */
} hs_phase_t;

typedef struct {
    uint16_t   gate_id;        /* the identity we expect and acknowledge   */
    hs_phase_t phase;
    uint32_t   frames;         /* total steps                              */
    uint32_t   since_announced; /* steps since HS_ANNOUNCED                 */
    uint32_t   linked_frames;  /* steps spent in HS_LINKED                 */
    uint16_t   gate_id_seen;   /* the identity the gate actually sent, 0 if none */

    /* Rotation evidence, gathered while LINKED. */
    bool     saw_id;        /* the gate identity came round again          */
    bool     saw_continue;  /* the 0xFFFF filler slot                      */
    bool     saw_idle;      /* a 0x0000 slot (UNK_2/3/END, or an empty ID) */
    bool     saw_data;      /* a rolling-counter slot (neither of the above)*/
    uint16_t rotation[9];   /* the last nine words seen while LINKED       */
    uint8_t  rot_count;     /* how many recorded (saturates)               */

    /* ---- rotation indexing, anchored on the identity slot ---- */
    bool     anchored;      /* have we seen the identity while LINKED?     */
    uint8_t  slot;          /* which slot the next word belongs to         */
    uint32_t rotations;     /* completed rotations since anchoring         */
    uint32_t shape_ok;      /* rotations whose fixed slots were correct    */
    uint32_t shape_bad;     /* faults in the rotation being scored          */
    uint32_t shape_bad_total;

    /* ---- data counters ---- */
    uint16_t last_data0, last_data1;
    bool     have_data;
    bool     data0_moved, data1_moved;

    /* ---- chip observation ---- */
    uint16_t chip_seen;     /* the chip word observed in slot 5, 0 if none */
    uint32_t chip_rotations; /* rotations it has been present for          */
    bool     chip_cleared;  /* slot 5 returned to 0 afterwards             */
    uint32_t chip_events;   /* how many distinct chips arrived             */

    /* ---- relink ---- */
    bool     relink_enabled;/* probe teardown/relink at all?                */
    bool     relink_wanted; /* we have asked for a teardown                */
    bool     relink_done;   /* and linked a second time                    */
    uint32_t links;         /* how many times we reached LINKED            */

    uint32_t tests;         /* HS_T_* bits earned so far                   */
} hs_t;

/*
 * Opt in to the teardown-and-relink probe.
 *
 * Off by default because it deliberately perturbs the word stream: it sends the
 * acknowledge a second time to close the session, so anything modelling an
 * uninterrupted rotation sees the gate re-announce and concludes the rotation
 * broke. Callers that only want to watch a steady session leave it off; the
 * ROM turns it on because "can it be re-linked after a disconnect" is a real
 * question about a device you unplug.
 */
static inline void hs_enable_relink(hs_t *h) { h->relink_enabled = true; }

static inline void hs_init(hs_t *h, uint16_t gate_id) {
    h->gate_id         = gate_id;
    h->phase           = HS_IDLE;
    h->frames          = 0;
    h->since_announced = 0;
    h->linked_frames   = 0;
    h->gate_id_seen    = 0;
    h->saw_id          = false;
    h->saw_continue    = false;
    h->saw_idle        = false;
    h->saw_data        = false;
    for (int i = 0; i < 9; i++) {
        h->rotation[i] = 0;
    }
    h->rot_count       = 0;
    h->anchored        = false;
    h->slot            = 0;
    h->rotations       = 0;
    h->shape_ok        = 0;
    h->shape_bad       = 0;
    h->shape_bad_total = 0;
    h->last_data0      = 0;
    h->last_data1      = 0;
    h->have_data       = false;
    h->data0_moved     = false;
    h->data1_moved     = false;
    h->chip_seen       = 0;
    h->chip_rotations  = 0;
    h->chip_cleared    = false;
    h->chip_events     = 0;
    h->relink_enabled  = false;
    h->relink_wanted   = false;
    h->relink_done     = false;
    h->links           = 0;
    h->tests           = 0;
}

/*
 * Fold one observed word into the rotation model.
 *
 * Kept separate from hs_step() because it is the part that has to be right:
 * the anchor, the slot index and the chip lifetime are what turn "nine words
 * went past" into "the documented rotation, carrying the chip the firmware
 * said it sent".
 */
static inline void hs_observe(hs_t *h, uint16_t answer) {
    /* Anchor, or re-anchor, whenever the identity comes round. */
    if (answer == h->gate_id) {
        if (h->anchored && h->slot != HS_SLOT_ID) {
            /* The identity turned up somewhere unexpected: the rotation is not
             * what we think it is. Re-anchor rather than score nonsense. */
            h->shape_bad++;
            h->shape_bad_total++;
        }
        h->anchored = true;
        h->slot     = HS_SLOT_ID;
    } else if (!h->anchored) {
        return;   /* nothing meaningful can be said yet */
    }

    uint8_t slot = h->slot;

    switch (slot) {
    case HS_SLOT_FILL1:
    case HS_SLOT_FILL2:
        if (answer != HS_WORD_CONTINUE) {
            h->shape_bad++;
            h->shape_bad_total++;
        }
        break;

    case HS_SLOT_END0:
    case HS_SLOT_END1:
    case HS_SLOT_END2:
        if (answer != HS_WORD_IDLE) {
            h->shape_bad++;
            h->shape_bad_total++;
        }
        break;

    case HS_SLOT_DATA0:
        if (h->have_data && answer != h->last_data0) {
            h->data0_moved = true;
        }
        h->last_data0 = answer;
        break;

    case HS_SLOT_DATA1:
        if (h->have_data && answer != h->last_data1) {
            h->data1_moved = true;
        }
        h->last_data1 = answer;
        h->have_data  = true;
        break;

    case HS_SLOT_CHIP:
        if (answer != HS_WORD_IDLE) {
            /*
             * A new chip is signalled by the slot having gone empty, not by a
             * different value: the firmware may legitimately send the same
             * chip twice, and comparing values alone merges the two into one
             * impossibly long delivery. That is exactly what happened the
             * first time this ran against the real gate -- 18 repeats instead
             * of two runs of 9.
             */
            if (h->chip_cleared || answer != h->chip_seen) {
                h->chip_seen      = answer;
                h->chip_rotations = 1;
                h->chip_cleared   = false;
                h->chip_events++;
                h->tests |= HS_T_CHIP;
            } else {
                h->chip_rotations++;
            }
        } else if (h->chip_seen && !h->chip_cleared) {
            /*
             * The chip retired. It must have been sent exactly
             * HS_CHIP_REPEATS times -- fewer means the gate dropped it early,
             * more means it never retires and the slot is wedged, which is the
             * "device stays busy" failure.
             */
            h->chip_cleared = true;
            if (h->chip_rotations == HS_CHIP_REPEATS) {
                h->tests |= HS_T_REPEATS;
            }
        }
        break;

    default:
        break;
    }

    if (slot == HS_SLOT_END2) {
        h->rotations++;
        if (h->shape_bad == 0) {
            h->shape_ok++;
        }
        /* Per rotation, not cumulative: one bad rotation -- the one straddling
         * a deliberate teardown, for instance -- must not stop every later
         * rotation from counting. */
        h->shape_bad = 0;
        if (h->shape_ok >= 2) {
            h->tests |= HS_T_SHAPE;
        }
        if (h->data0_moved && h->data1_moved) {
            h->tests |= HS_T_COUNTERS;
        }
        h->slot = HS_SLOT_ID;
    } else {
        h->slot = (uint8_t)(slot + 1);
    }

    if (h->linked_frames >= HS_LINK_EVIDENCE_FRAMES && h->gate_id_seen == h->gate_id) {
        h->tests |= HS_T_LINK;
    }
}

/*
 * Advance one frame. `answer` is the word the gate put on the wire (SIOMULTI0).
 * Returns the word to stage for the gate this frame (SIOMLT_SEND).
 */
static inline uint16_t hs_step(hs_t *h, uint16_t answer) {
    h->frames++;

    switch (h->phase) {
    case HS_IDLE:
        /* The gate holds its identity out continuously until it sees the sync
         * word, so seeing it is how we know a gate is really there. */
        if (answer == h->gate_id) {
            h->phase           = HS_ANNOUNCED;
            h->gate_id_seen    = answer;
            h->since_announced = 0;
        }
        return HS_WORD_IDLE;

    case HS_ANNOUNCED:
        /* Give the announcement a couple of frames to settle -- matches the
         * "frames > 2" guard in broker.run_gate() -- then send the sync word. */
        h->since_announced++;
        if (h->since_announced > 2) {
            h->phase = HS_SYNCED;
            return HS_WORD_SYNC;
        }
        return HS_WORD_IDLE;

    case HS_SYNCED:
        /* The acknowledge equals the gate identity for every gate observed so
         * far (see src/chipgate.c). Sending it opens the session. */
        h->phase = HS_LINKED;
        h->links++;
        if (h->links >= 2) {
            h->relink_done = true;
            h->tests |= HS_T_RELINK;
        }
        h->anchored = false;   /* re-anchor on the next identity */
        return h->gate_id;

    case HS_LINKED:
    default:
        h->linked_frames++;
        hs_observe(h, answer);

        /*
         * Once the first pass is complete, tear the session down and do it
         * again. Sending the acknowledge a second time is what closes a
         * session (see chipgate_step), and a gate that cannot be re-linked
         * after a disconnect is a gate that needs a power cycle in the middle
         * of a play session.
         */
        if (h->relink_enabled && !h->relink_wanted &&
            (h->tests & (HS_T_LINK | HS_T_SHAPE | HS_T_COUNTERS)) ==
                (HS_T_LINK | HS_T_SHAPE | HS_T_COUNTERS) &&
            (h->chip_cleared || h->linked_frames > HS_LINK_EVIDENCE_FRAMES * 8)) {
            h->relink_wanted = true;
            h->phase = HS_IDLE;      /* watch for the announcement again */
            h->since_announced = 0;
            return h->gate_id;       /* the teardown word */
        }
        h->rotation[h->rot_count % 9] = answer;
        if (h->rot_count < 0xFF) {
            h->rot_count++;
        }
        if (answer == h->gate_id) {
            h->saw_id = true;
        } else if (answer == HS_WORD_CONTINUE) {
            h->saw_continue = true;
        } else if (answer == HS_WORD_IDLE) {
            h->saw_idle = true;
        } else {
            h->saw_data = true;
        }
        /* Idle from here; every word we send advances the gate's rotation. */
        return HS_WORD_IDLE;
    }
}

/*
 * The session is up and behaving: we reached LINKED, held it long enough to see
 * the rotation cycle, and observed the gate identity, the 0xFFFF filler and at
 * least one of the varied slots (a data counter or a 0x0000 slot). That is the
 * "announces FF00 then the documented 9-slot rotation" check, made from the
 * game's side.
 */
static inline bool hs_passed(const hs_t *h) {
    return h->phase == HS_LINKED
        && h->linked_frames >= HS_LINK_EVIDENCE_FRAMES
        && h->saw_id
        && h->saw_continue
        && (h->saw_idle || h->saw_data);
}

/* Every check earned, including the two that need the firmware to send a chip. */
static inline bool hs_all_passed(const hs_t *h) {
    return (h->tests & HS_T_ALL) == HS_T_ALL;
}

/*
 * Everything that does not depend on the firmware doing something. A gate that
 * links, rotates correctly and can be re-linked is working even if nobody has
 * pressed the button to send a chip, and reporting that as a failure would
 * train people to ignore the result.
 */
static inline bool hs_protocol_passed(const hs_t *h) {
    uint32_t want = HS_T_ALL & ~HS_T_NEEDS_FIRMWARE;
    if (!h->relink_enabled) {
        want &= ~HS_T_RELINK;   /* not probed, so not expected */
    }
    return (h->tests & want) == want;
}

static inline const char *hs_test_name(uint32_t bit) {
    switch (bit) {
    case HS_T_LINK:     return "link";
    case HS_T_SHAPE:    return "rotation shape";
    case HS_T_COUNTERS: return "data counters";
    case HS_T_CHIP:     return "chip delivered";
    case HS_T_REPEATS:  return "chip repeats";
    case HS_T_RELINK:   return "relink";
    default:            return "?";
    }
}

#endif /* ROM_HANDSHAKE_H */
