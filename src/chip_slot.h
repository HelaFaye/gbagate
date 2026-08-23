/*
 * chip_slot.h -- drive the shared Battle Chip pad bus.
 *
 * Both chip families land on the same eleven expander lines, because the union
 * of the pads either family uses is pads 1..11:
 *
 *     Advance / Progress   common pad 10, ID bits on pads 1-9 and 11
 *     Beast Link           common pad  2, ID bits on pads 3-11
 *
 * The overlap is the point: pad 10 is Advance's common and a Beast ID bit, and
 * pad 2 is Beast's common and an Advance ID bit. Since only one chip is ever
 * inserted at a time, both connectors can wire to the same eleven lines and the
 * driver simply pulls whichever pad is common for the occupied slot.
 *
 *     11 pad lines + 2 detect switches = 13 of an MCP23017's 16.
 *
 * Bus discipline
 * --------------
 * Every line idles as an input with a pull-up. Exactly one line is driven low,
 * only for the duration of a read, and released afterwards. Leaving a common
 * pad driven would short against the other slot's chip the moment it is
 * inserted, so the release is not optional tidiness.
 *
 * The expander access is injected so the sequencing can be tested on a host,
 * including the failure paths that are awkward to provoke on hardware.
 */

#ifndef CHIP_SLOT_H
#define CHIP_SLOT_H

#include <stdbool.h>
#include <stdint.h>

#include "chip_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pads 1..11 occupy bits 0..10 of every mask below. */
#define SLOT_PAD_MASK 0x07FFu
#define SLOT_PAD_BIT(pad) ((uint16_t)(1u << ((pad) - 1)))

/*
 * set_dir: 1 = input with pull-up, 0 = output.
 * set_out: level for lines currently configured as outputs.
 * get_in:  read the whole port.
 * Each returns false on a bus error, which propagates as SLOT_SCAN_IO_ERROR.
 */
typedef struct {
    bool (*set_dir)(void *ctx, uint16_t input_mask);
    bool (*set_out)(void *ctx, uint16_t levels);
    bool (*get_in)(void *ctx, uint16_t *levels);
} chip_bus_io_t;

typedef enum {
    SLOT_SCAN_EMPTY = 0,  /* neither detect switch closed        */
    SLOT_SCAN_OK,         /* one slot occupied, pads read        */
    SLOT_SCAN_CONFLICT,   /* both occupied: the bus is shared    */
    SLOT_SCAN_IO_ERROR    /* the expander did not answer         */
} slot_scan_t;

typedef struct {
    chip_bus_io_t io;
    void         *ctx;

    /* diagnostics */
    uint32_t scans;
    uint32_t conflicts;
    uint32_t io_errors;
    uint16_t last_pads;
    chip_family_t last_family;
} chip_slots_t;

void chip_slots_init(chip_slots_t *s, const chip_bus_io_t *io, void *ctx);

/* Which pad is driven low for a family. */
uint8_t chip_slot_common_pad(chip_family_t family);

/* Mask of pads carrying ID bits for a family. */
uint16_t chip_slot_id_mask(chip_family_t family);

/*
 * Read whichever slot is occupied.
 *
 * det_advance / det_beast are the mechanical detect switches, already
 * debounced by the caller, true meaning a chip is seated.
 *
 * On SLOT_SCAN_OK, *family and *pads are set and *pads is ready for
 * chip_reader_decode(). Every line is left as an input before returning,
 * including on the error paths.
 */
slot_scan_t chip_slots_scan(chip_slots_t *s, bool det_advance, bool det_beast,
                            chip_family_t *family, uint16_t *pads);

#ifdef __cplusplus
}
#endif

#endif /* CHIP_SLOT_H */
