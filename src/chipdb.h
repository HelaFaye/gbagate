/*
 * chipdb.h -- read-only accessor for a .chdb blob produced by build_chipdb.py.
 *
 * Designed to be used in place: point it at flash (or a memory-mapped file) and
 * it copies nothing. Records are fixed 16 bytes so lookups are pointer
 * arithmetic, and the wire-ID map answers the question the chip reader actually
 * asks -- given the number formed by a chip's shorted pads, which record is it?
 *
 * All multi-byte fields are little-endian, which matches both the ESP32 and any
 * host you would test on, so the loads are direct.
 */

#ifndef CHIPDB_H
#define CHIPDB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHIPDB_MAGIC   "CHDB"
#define CHIPDB_VERSION 2
#define CHIPDB_NO_RECORD 0xFFFFu
#define CHIPDB_NO_ID     0xFFFFu

typedef enum {
    CHIP_ELEM_NONE = 0, CHIP_ELEM_FIRE, CHIP_ELEM_AQUA,
    CHIP_ELEM_ELEC, CHIP_ELEM_WOOD
} chip_element_t;

typedef enum {
    CHIP_LIB_NONE = 0, CHIP_LIB_STANDARD, CHIP_LIB_MEGA,
    CHIP_LIB_GIGA, CHIP_LIB_DARK, CHIP_LIB_PA
} chip_library_t;

#define CHIP_FLAG_IN_LIBRARY 0x01
#define CHIP_FLAG_SHOW_DAMAGE 0x02

#define CHIP_CODE_NONE 0xFFu
#define CHIP_CODE_STAR 26u   /* the '*' wildcard code */

typedef struct {
    uint16_t index;      /* chip table index */
    uint8_t  element;
    uint8_t  library;
    uint8_t  mb;
    uint8_t  gate_uses;  /* Chip Gate Use Limit: 1 or 3 */
    uint16_t damage;
    uint16_t lib_number; /* position within its library */
    uint8_t  rarity;     /* stars, 1..5 */
    uint8_t  flags;
    uint8_t  codes[4];   /* 0..25 = A..Z, 26 = '*', 0xFF = none */
} chip_record_t;

typedef struct {
    const uint8_t *base;
    size_t         size;
    uint16_t       count;
    uint16_t       max_id;
    const uint8_t *records;
    const uint32_t *name_offsets;
    const uint16_t *idmap;
    const char    *pool;
    char           game[8];
} chipdb_t;

/*
 * Validate and attach. Returns false on a bad magic, a version mismatch, or any
 * section that would run off the end -- worth checking, because a truncated
 * flash write otherwise shows up much later as garbage chip names.
 */
bool chipdb_open(chipdb_t *db, const void *blob, size_t size);

static inline uint16_t chipdb_count(const chipdb_t *db) { return db->count; }
static inline const char *chipdb_game(const chipdb_t *db) { return db->game; }

/* Fetch by record slot, 0 .. count-1. */
bool chipdb_get(const chipdb_t *db, uint16_t slot, chip_record_t *out);

/* Name for a record slot. NULL if out of range. */
const char *chipdb_name(const chipdb_t *db, uint16_t slot);

/*
 * Resolve a wire ID -- the value the reader decodes from the pads, and the
 * value slot 5 of the gate rotation carries -- to a record slot.
 * Returns CHIPDB_NO_RECORD when the ID is outside the map or unmapped.
 */
uint16_t chipdb_slot_for_id(const chipdb_t *db, uint16_t chip_id);

/*
 * The inverse: given a record slot, the wire ID that resolves to it.
 *
 * This is the value the gate must put in slot 5 of the rotation. It is NOT
 * chip_record_t.index -- that is the chip table's "ID Sort" key, a different
 * numbering that happens to agree with the wire ID for some chips and not
 * others, so sending it looks plausible while naming the wrong chip.
 *
 * build_chipdb.py assigns each table entry to at most one wire ID (the `used`
 * set enforces exclusivity), so the map is injective and this inverse is
 * unambiguous. It is a linear scan of at most max_id + 1 entries, which is a
 * few hundred, and it runs once when the user slots a chip in -- not on the
 * link's hot path.
 *
 * Returns CHIPDB_NO_ID when no wire ID maps to this slot. That is a real case:
 * a database carries more records than the reader has IDs for, and those chips
 * simply cannot be put on the wire. Refusing is correct; sending some other
 * number is not.
 */
uint16_t chipdb_id_for_slot(const chipdb_t *db, uint16_t slot);

/* Convenience: wire ID straight to a record. */
bool chipdb_get_by_id(const chipdb_t *db, uint16_t chip_id, chip_record_t *out);

/* 'A'..'Z', '*', or 0 when the slot is unused. */
char chipdb_code_char(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif /* CHIPDB_H */
