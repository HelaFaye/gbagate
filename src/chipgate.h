/*
 * chipgate.h -- Battle Chip Gate link-layer responder (transport agnostic).
 *
 * Emulates the slave half of the Capcom Battle Chip Gate / Progress Chip Gate /
 * Beast Link Gate as seen by Mega Man Battle Network 4, 4.5, 5 and 6.
 *
 * This header contains no hardware dependencies on purpose. Feed it the 16-bit
 * word the game sent this frame, take back the 16-bit word to put on the wire.
 * That lets the same object file run against real GBA hardware (via SIOMLT_SEND
 * on an ESP32 bit-bang PHY), against GBE+ over TCP, or against a unit test.
 *
 * Derivation
 * ----------
 * The 9-slot response rotation and the handshake below were recovered by
 * disassembling Greiga Master's Battle Chip Gate Homebrew (handler at
 * 0x080B3940, state block at 0x02000000) and cross-checked against mGBA's
 * independent implementation in src/gba/extra/battlechip.c, which agrees on
 * both the slot ordering and the gate identity constants.
 */

#ifndef CHIPGATE_H
#define CHIPGATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Wire constants -------------------------------------------------- */

/* Gate identity words. Which one you use decides which game will talk to you;
 * BN6 is split by region because the two builds expect different gates. */
#define CG_ID_BATTLECHIP    0xFFC6u /* BN4 / EXE4 / EXE4.5  Battle Chip Gate  */
#define CG_ID_PROGRESS      0xFFC7u /* BN5 / EXE5           Progress Chip Gate*/
#define CG_ID_BEAST_LINK    0xFFC4u /* EXE6      (JP)       Beast Link Gate   */
#define CG_ID_BEAST_LINK_US 0xFF00u /* MMBN6     (US/EU)    Beast Link Gate   */

#define CG_WORD_CONTINUE 0xFFFFu /* filler / "gate present, nothing to say"   */
#define CG_WORD_SYNC     0x8FFFu /* game -> gate: are you there?              */
#define CG_CHIP_NONE     0x0000u /* empty chip slot                           */

/* Response rotation. One word leaves the gate per multiplayer frame; the
 * sequence repeats every 9 frames (~150 ms at 60 Hz). Names follow mGBA. */
enum {
    CG_SLOT_COMMAND = 0, /* gate identity                                    */
    CG_SLOT_UNK_0   = 1, /* 0xFFFF                                           */
    CG_SLOT_UNK_1   = 2, /* 0xFFFF                                           */
    CG_SLOT_DATA_0  = 3, /* rolling counter A                                */
    CG_SLOT_DATA_1  = 4, /* rolling counter B                                */
    CG_SLOT_ID      = 5, /* the slotted chip                                 */
    CG_SLOT_UNK_2   = 6, /* 0x0000                                           */
    CG_SLOT_UNK_3   = 7, /* 0x0000                                           */
    CG_SLOT_END     = 8  /* 0x0000                                           */
};

/* A chip stays on the wire for this many rotations before the slot empties.
 * 9 rotations x 9 slots = 81 frames, about 1.35 s of "chip inserted". */
#define CG_CHIP_REPEATS 9

typedef enum {
    CG_GAME_BN4 = 0,  /* MMBN4, Rockman EXE 4, Rockman EXE 4.5 (all regions) */
    CG_GAME_BN5,      /* MMBN5, Rockman EXE 5 (all regions)                  */
    CG_GAME_BN6_JP,   /* Rockman EXE 6 Gregar / Falzar                       */
    CG_GAME_BN6_US,   /* MMBN6 Cybeast Gregar / Falzar                       */
    CG_GAME_COUNT
} cg_game_t;

typedef enum {
    CG_LINK_IDLE = 0,  /* holding the gate ID out, waiting for 0x8FFF        */
    CG_LINK_SYNCED,    /* saw the ping, waiting for the game's acknowledge   */
    CG_LINK_ACTIVE     /* session up, rotating through the 9 slots           */
} cg_link_state_t;

typedef struct {
    /* configuration */
    uint16_t gate_id;   /* what we announce ourselves as                     */
    uint16_t ack_word;  /* what the game echoes to open (and close) a session*/
    bool     enabled;   /* false => go transparent, emit 0xFFFF only         */

    /* live state */
    cg_link_state_t state;
    uint8_t  slot;        /* 0..8, index into the response rotation          */
    uint16_t chip_id;     /* currently slotted chip, CG_CHIP_NONE if empty   */
    uint8_t  chip_reps;   /* rotations the current chip has been sent for    */
    uint16_t data0;       /* CG_SLOT_DATA_0 counter                          */
    uint16_t data1;       /* CG_SLOT_DATA_1 counter                          */

    /* observability -- useful on a 128x128 status bar */
    bool     chip_consumed; /* set once when a chip finishes transmitting    */
    uint32_t frames;        /* total frames serviced                         */
    uint32_t sessions;      /* successful handshakes since reset             */
} chipgate_t;

/* ---- API ------------------------------------------------------------- */

/* Configure for a game. Safe to call again to switch games; drops any session. */
void chipgate_init(chipgate_t *cg, cg_game_t game);

/* Override the identity directly, for experimenting with undocumented gates. */
void chipgate_set_identity(chipgate_t *cg, uint16_t gate_id, uint16_t ack_word);

/* Tear down the session but keep configuration and the slotted chip. */
void chipgate_reset_link(chipgate_t *cg);

/*
 * Service one multiplayer frame.
 *
 * from_game: the word the master (the GBA) sent us this frame -- on real
 *            hardware, SIOMULTI0.
 * returns:   the word to stage for the next transfer -- SIOMLT_SEND.
 *
 * Must be called exactly once per completed transfer. On hardware the return
 * value has to be in the shift register before the master clocks our slot, so
 * call this from the transfer-complete path, not from the UI task.
 */
uint16_t chipgate_step(chipgate_t *cg, uint16_t from_game);

/*
 * Slot in a chip. chip_id is the raw value the game expects, which for a
 * physical Battle Chip is literally the binary number formed by its shorted
 * edge pads (pad 10 common on 11-pad Advance/Progress chips, pad 2 common on
 * 12-pad Beast chips).
 *
 * Returns false if a chip is still transmitting -- the gate holds one chip at
 * a time, same as the real hardware.
 */
bool chipgate_load(chipgate_t *cg, uint16_t chip_id);

/* Yank the current chip immediately instead of waiting out its repeats. */
void chipgate_clear(chipgate_t *cg);

static inline bool chipgate_busy(const chipgate_t *cg) {
    return cg->chip_id != CG_CHIP_NONE;
}

static inline bool chipgate_connected(const chipgate_t *cg) {
    return cg->state == CG_LINK_ACTIVE;
}

/* Consume the "a chip finished sending" edge. Returns true at most once per
 * chip; good for driving a click or an LED without polling chip_id. */
bool chipgate_take_consumed(chipgate_t *cg);

const char *chipgate_game_name(cg_game_t game);
const char *chipgate_state_name(cg_link_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* CHIPGATE_H */
