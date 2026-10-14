/*
 * chip_reader.h -- decode a physical Battle Chip from its edge pads.
 *
 * A Battle Chip contains no silicon. It is a PCB with shorting bars between a
 * common pad and some subset of the others, and the chip's identity is simply
 * the binary number those shorts form. Confirmed across all 58 schematics in
 * the EXE chip project:
 *
 *   Advance / Progress  11 pads, common = pad 10 (present in 24 of 24 nets),
 *                       ID bits on pads 1-9 -> 9 bits. Pad 11 is *not* an ID
 *                       bit: see below.
 *   Beast Link          12 pads, common = pad 2  (23 of 23), ID bits on pads
 *                       3-11 -> 9 bits. Pads 1 and 12 are never wired.
 *
 * So reading one is: drive the common pad low, read the rest as inputs with
 * pull-ups, and a pad that reads low is shorted. Eleven lines is a lot of GPIO
 * next to a display and a link port, so this takes the pad word from a
 * callback -- point it at an MCP23017 or a 74HC165 chain and spend two pins.
 *
 * Bit order, settled
 * ------------------
 * Solved from the EXE chip project schematics against our own chip databases,
 * which is better evidence than one chip and a multimeter because it is eight
 * independent chips at once:
 *
 *   pad 1 = bit 8 (MSB) ... pad 9 = bit 0 (LSB), pad 10 = common,
 *   pad 11 = family marker, carries no weight.
 *
 * The Gun Del Sol family settles the weighting by itself. Its three chips are
 * consecutive IDs and differ by one pad each:
 *
 *   GunDelS1  pads {4}      = 32     pad 4 = 32
 *   GunDelS2  pads {4,9}    = 33     so pad 9 = 1
 *   GunDelS3  pads {4,8}    = 34     so pad 8 = 2
 *
 * Confirmed on GunDelEX (219), Django[SP] (279), Django[DS] (280), Otenko
 * (215) and, from the other family, LifeSync (28). Eight for eight.
 *
 * Pad 11 is what distinguishes Progress chips from Advance ones: every EXE 5
 * Progress chip ties it, no EXE 4 Advance chip does. Treating it as a tenth
 * bit shifts every weight and yields plausible-looking wrong IDs -- which is
 * exactly what this file used to do.
 *
 * Beast Link, checked the same way
 * --------------------------------
 * Solved against the EXE 6 Link schematics and the bn6g database, seven chips
 * for seven:
 *
 *   BugBomb 67, Discord 147, Django 278, Falzar 313, GunDelS3 17,
 *   HeatDrgn 46, TrnArrw3 26
 *
 * And the orientation is the *opposite* of Advance, which is not something
 * anyone would have guessed:
 *
 *   Advance/Progress   pad 1 = bit 8 (MSB) ... pad 9 = bit 0
 *   Beast Link         pad 3 = bit 0 (LSB) ... pad 11 = bit 8
 *
 *   BugBomb  {3,4,9}      = 1 + 2 + 64        =  67
 *   Discord  {3,4,7,10}   = 1 + 2 + 16 + 128  = 147
 *   GunDelS3 {3,7}        = 1 + 16            =  17
 *
 * The other direction scores 1 of 7, and the one it gets is Falzar, whose pad
 * set is a palindrome under reversal. A single agreeing chip is exactly what a
 * wrong bit order looks like -- which is why seven were checked and why the
 * first attempt at this, with only Falzar to go on, would have been wrong.
 *
 * msb_is_lowest_pad is therefore set from the family rather than fixed, and
 * both values are confirmed rather than assumed.
 */

#ifndef CHIP_READER_H
#define CHIP_READER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHIP_FAMILY_ADVANCE = 0, /* BN4 Advance, BN5 Progress: 11 pads */
    CHIP_FAMILY_BEAST   = 1  /* BN6 Beast Link: 12 pads             */
} chip_family_t;

#define CHIP_ADV_PADS    11
#define CHIP_ADV_COMMON  10
#define CHIP_BEAST_PADS  12
#define CHIP_BEAST_COMMON 2

/* Returned when a slot holds nothing, or holds a chip with no shorts at all --
 * see the note on chip_reader_poll(). */
#define CHIP_ID_NONE 0xFFFFu

/*
 * Read the slot's pads. Bit N of the return is pad N+1: 1 = open (pulled high),
 * 0 = shorted to the common pad. The common pad's own bit is ignored.
 */
typedef uint16_t (*chip_pads_read_fn)(void *ctx);

typedef enum {
    CHIP_EVENT_NONE = 0,
    CHIP_EVENT_INSERTED,
    CHIP_EVENT_REMOVED
} chip_event_t;

typedef struct {
    /* config */
    chip_family_t     family;
    bool              msb_is_lowest_pad; /* see calibration note above */
    uint8_t           stable_samples;    /* consecutive matches required */
    chip_pads_read_fn read;
    void             *ctx;
    bool              has_detect_switch; /* true if presence is sensed apart
                                          * from the pads -- see poll() */

    /* live state */
    uint16_t candidate;      /* raw pad word being debounced */
    uint8_t  matches;        /* how many consecutive samples agreed */
    uint16_t settled;        /* raw pad word currently accepted */
    bool     occupied;
    uint16_t chip_id;        /* decoded, valid while occupied */
    uint32_t reads;
    uint32_t bounces;        /* samples that broke a run -- contact quality */
} chip_reader_t;

void chip_reader_init(chip_reader_t *r, chip_family_t family,
                      chip_pads_read_fn read, void *ctx);

/* Decode a raw pad word to an ID. Exposed for calibration and tests. */
uint16_t chip_reader_decode(const chip_reader_t *r, uint16_t pads);

/*
 * Sample once. Call at roughly 50 Hz; contacts settle well inside two samples
 * and this keeps the UI responsive without hammering an I2C expander.
 *
 * present is the mechanical detect switch, if the slot has one. It matters:
 * a blank Advance chip has no shorts, so its pad word is identical to an empty
 * slot, and the EXE chip project's own Advance_Base_Chip is exactly that. With
 * no detect switch, ID 0 is unreadable. Pass true always if has_detect_switch
 * is false and accept that limitation.
 */
chip_event_t chip_reader_poll(chip_reader_t *r, bool present);

static inline bool chip_reader_occupied(const chip_reader_t *r) {
    return r->occupied;
}

static inline uint16_t chip_reader_id(const chip_reader_t *r) {
    return r->occupied ? r->chip_id : CHIP_ID_NONE;
}

uint8_t chip_reader_pad_count(chip_family_t family);
uint8_t chip_reader_common_pad(chip_family_t family);
uint8_t chip_reader_id_bits(chip_family_t family);

#ifdef __cplusplus
}
#endif

#endif /* CHIP_READER_H */
