/*
 * chip_slot.c -- see chip_slot.h
 */

#include <string.h>

#include "chip_slot.h"

uint8_t chip_slot_common_pad(chip_family_t family) {
    return chip_reader_common_pad(family);
}

uint16_t chip_slot_id_mask(chip_family_t family) {
    uint16_t mask = 0;
    uint8_t common = chip_slot_common_pad(family);
    uint8_t lo = (family == CHIP_FAMILY_BEAST) ? 3 : 1;
    uint8_t hi = 11;
    for (uint8_t pad = lo; pad <= hi; pad++) {
        if (pad != common) {
            mask |= SLOT_PAD_BIT(pad);
        }
    }
    return mask;
}

void chip_slots_init(chip_slots_t *s, const chip_bus_io_t *io, void *ctx) {
    memset(s, 0, sizeof *s);
    s->io  = *io;
    s->ctx = ctx;
    s->last_family = CHIP_FAMILY_ADVANCE;
}

/* Return every line to input-with-pull-up. Called on all exits. */
static bool release_bus(chip_slots_t *s) {
    return s->io.set_dir && s->io.set_dir(s->ctx, SLOT_PAD_MASK);
}

slot_scan_t chip_slots_scan(chip_slots_t *s, bool det_advance, bool det_beast,
                            chip_family_t *family, uint16_t *pads) {
    s->scans++;

    if (!s->io.set_dir || !s->io.set_out || !s->io.get_in) {
        s->io_errors++;
        return SLOT_SCAN_IO_ERROR;
    }

    if (det_advance && det_beast) {
        /* One shared bus cannot serve two chips. Refuse rather than read a
         * blend of both, which would decode to a plausible wrong ID. */
        s->conflicts++;
        release_bus(s);
        return SLOT_SCAN_CONFLICT;
    }

    if (!det_advance && !det_beast) {
        release_bus(s);
        return SLOT_SCAN_EMPTY;
    }

    chip_family_t fam = det_beast ? CHIP_FAMILY_BEAST : CHIP_FAMILY_ADVANCE;
    uint16_t common_bit = SLOT_PAD_BIT(chip_slot_common_pad(fam));

    /*
     * Set the level before the direction, so the line is never briefly driven
     * high: the other slot's pads sit on the same copper.
     */
    if (!s->io.set_out(s->ctx, (uint16_t)(SLOT_PAD_MASK & ~common_bit))) {
        s->io_errors++;
        release_bus(s);
        return SLOT_SCAN_IO_ERROR;
    }
    if (!s->io.set_dir(s->ctx, (uint16_t)(SLOT_PAD_MASK & ~common_bit))) {
        s->io_errors++;
        release_bus(s);
        return SLOT_SCAN_IO_ERROR;
    }

    uint16_t levels = 0;
    bool ok = s->io.get_in(s->ctx, &levels);

    /* Release before reporting anything, error or not. */
    if (!release_bus(s) || !ok) {
        s->io_errors++;
        return SLOT_SCAN_IO_ERROR;
    }

    /*
     * The common pad reads low because we are driving it, which is not
     * information. Force it high so chip_reader_decode() ignores it, and force
     * any pad outside this family's ID set high too -- a Beast chip leaves pads
     * 1 and 12 unconnected, and floating inputs must not become ID bits.
     */
    uint16_t id_mask = chip_slot_id_mask(fam);
    uint16_t clean = (uint16_t)((levels & id_mask) | (SLOT_PAD_MASK & ~id_mask));

    s->last_pads   = clean;
    s->last_family = fam;

    if (family) {
        *family = fam;
    }
    if (pads) {
        *pads = clean;
    }
    return SLOT_SCAN_OK;
}
