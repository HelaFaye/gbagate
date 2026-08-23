/*
 * chip_reader.h -- decode a physical Battle Chip from its edge pads.
 *
 * A Battle Chip contains no silicon. It is a PCB with shorting bars between a
 * common pad and some subset of the others, and the chip's identity is simply
 * the binary number those shorts form. Confirmed across all 58 schematics in
 * the EXE chip project:
 *
 *   Advance / Progress  11 pads, common = pad 10 (present in 24 of 24 nets),
 *                       ID bits on pads 1-9 and 11 -> 10 bits.
 *   Beast Link          12 pads, common = pad 2  (23 of 23), ID bits on pads
 *                       3-11 -> 9 bits. Pads 1 and 12 are never wired.
 *
 * So reading one is: drive the common pad low, read the rest as inputs with
 * pull-ups, and a pad that reads low is shorted. Eleven lines is a lot of GPIO
 * next to a display and a link port, so this takes the pad word from a
 * callback -- point it at an MCP23017 or a 74HC165 chain and spend two pins.
 *
 * Bit order is calibratable on purpose
 * ------------------------------------
 * That the pads form the ID is established. Which pad is the most significant
 * bit is not, and getting it backwards yields plausible-looking wrong IDs. One
 * chip whose ID you know settles it: read it both ways and keep the order that
 * matches. Until then, treat decoded IDs as provisional.
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
