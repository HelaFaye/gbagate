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
} hs_t;

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
    h->rot_count = 0;
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
        return h->gate_id;

    case HS_LINKED:
    default:
        h->linked_frames++;
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

#endif /* ROM_HANDSHAKE_H */
