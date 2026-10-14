/*
 * chip_reader.c -- see chip_reader.h
 */

#include <string.h>

#include "chip_reader.h"

#define DEFAULT_STABLE_SAMPLES 3

uint8_t chip_reader_pad_count(chip_family_t family) {
    return family == CHIP_FAMILY_BEAST ? CHIP_BEAST_PADS : CHIP_ADV_PADS;
}

uint8_t chip_reader_common_pad(chip_family_t family) {
    return family == CHIP_FAMILY_BEAST ? CHIP_BEAST_COMMON : CHIP_ADV_COMMON;
}

/*
 * Beast chips never wire pads 1 and 12, so only pads 3..11 carry information.
 * Advance chips use everything except the common pad.
 */
static bool pad_carries_id(chip_family_t family, uint8_t pad) {
    if (pad == chip_reader_common_pad(family)) {
        return false;
    }
    if (family == CHIP_FAMILY_BEAST) {
        return pad >= 3 && pad <= 11;
    }
    /*
     * Pads 1..9 only. Pad 10 is the common (excluded above) and pad 11 is not
     * an ID bit at all -- see the note in chip_reader.h. Counting it as one
     * gave a 10-bit value and shifted every weight, so IDs came out plausible
     * and wrong.
     */
    return pad >= 1 && pad <= 9;
}

uint8_t chip_reader_id_bits(chip_family_t family) {
    uint8_t n = 0;
    for (uint8_t pad = 1; pad <= chip_reader_pad_count(family); pad++) {
        if (pad_carries_id(family, pad)) {
            n++;
        }
    }
    return n;
}

void chip_reader_init(chip_reader_t *r, chip_family_t family,
                      chip_pads_read_fn read, void *ctx) {
    memset(r, 0, sizeof *r);
    r->family            = family;
    r->read              = read;
    r->ctx               = ctx;
    r->stable_samples    = DEFAULT_STABLE_SAMPLES;
    /*
     * The two families run in opposite directions, which is not something
     * anyone would guess:
     *
     *   Advance/Progress  pad 1 is the MSB  (lowest ID pad = most significant)
     *   Beast Link        pad 3 is the LSB  (lowest ID pad = least significant)
     *
     * Both solved from the EXE schematics against our own databases -- eight
     * chips for Advance, seven for Beast. See chip_reader.h.
     */
    r->msb_is_lowest_pad = (family != CHIP_FAMILY_BEAST);
    r->has_detect_switch = true; /* the slot has a mechanical switch, so a
                                  * blank chip (no shorts at all) reads as
                                  * ID 0 rather than as an empty slot */
    r->chip_id           = CHIP_ID_NONE;
}

uint16_t chip_reader_decode(const chip_reader_t *r, uint16_t pads) {
    uint8_t pad_count = chip_reader_pad_count(r->family);
    uint16_t id = 0;

    if (r->msb_is_lowest_pad) {
        for (uint8_t pad = 1; pad <= pad_count; pad++) {
            if (!pad_carries_id(r->family, pad)) {
                continue;
            }
            /* a pad reading low is shorted to common, so it is a set bit */
            uint16_t bit = (pads >> (pad - 1)) & 1u ? 0u : 1u;
            id = (uint16_t)((id << 1) | bit);
        }
    } else {
        for (uint8_t pad = pad_count; pad >= 1; pad--) {
            if (!pad_carries_id(r->family, pad)) {
                continue;
            }
            uint16_t bit = (pads >> (pad - 1)) & 1u ? 0u : 1u;
            id = (uint16_t)((id << 1) | bit);
        }
    }
    return id;
}

chip_event_t chip_reader_poll(chip_reader_t *r, bool present) {
    r->reads++;

    if (!present) {
        r->matches   = 0;
        r->candidate = 0xFFFFu;
        if (r->occupied) {
            r->occupied = false;
            r->chip_id  = CHIP_ID_NONE;
            r->settled  = 0xFFFFu;
            return CHIP_EVENT_REMOVED;
        }
        return CHIP_EVENT_NONE;
    }

    uint16_t pads = r->read ? r->read(r->ctx) : 0xFFFFu;

    if (r->matches == 0 || pads != r->candidate) {
        if (r->matches != 0) {
            r->bounces++;
        }
        r->candidate = pads;
        r->matches   = 1;
        return CHIP_EVENT_NONE;
    }

    if (r->matches < r->stable_samples) {
        r->matches++;
    }
    if (r->matches < r->stable_samples) {
        return CHIP_EVENT_NONE;
    }

    /* Stable. Has anything actually changed? */
    if (r->occupied && pads == r->settled) {
        return CHIP_EVENT_NONE;
    }

    /*
     * Without a detect switch, an all-open pad word is ambiguous: it is either
     * an empty slot or a chip with no shorts. Treat it as empty, which is the
     * safe reading -- a spurious "chip inserted" would put a bogus ID on the
     * link, while a missed blank chip only disappoints the user.
     */
    uint16_t id_mask = (uint16_t)((1u << chip_reader_pad_count(r->family)) - 1u);
    bool all_open = (pads & id_mask) == id_mask;
    if (all_open && !r->has_detect_switch) {
        if (r->occupied) {
            r->occupied = false;
            r->chip_id  = CHIP_ID_NONE;
            r->settled  = pads;
            return CHIP_EVENT_REMOVED;
        }
        r->settled = pads;
        return CHIP_EVENT_NONE;
    }

    r->settled  = pads;
    r->chip_id  = chip_reader_decode(r, pads);
    r->occupied = true;
    return CHIP_EVENT_INSERTED;
}
