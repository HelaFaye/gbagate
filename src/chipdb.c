/*
 * chipdb.c -- see chipdb.h
 */

#include <string.h>

#include "chipdb.h"

#define HDR_SIZE   24
#define REC_SIZE   16

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool chipdb_open(chipdb_t *db, const void *blob, size_t size) {
    memset(db, 0, sizeof *db);
    const uint8_t *p = (const uint8_t *)blob;

    if (!p || size < HDR_SIZE) {
        return false;
    }
    if (memcmp(p, CHIPDB_MAGIC, 4) != 0 || p[4] != CHIPDB_VERSION) {
        return false;
    }

    memcpy(db->game, p + 5, 7);
    db->game[7] = '\0';

    uint16_t count  = rd16(p + 12);
    uint16_t max_id = rd16(p + 14);
    uint32_t names_off = rd32(p + 16);
    uint32_t idmap_off = rd32(p + 20);

    size_t records_end = (size_t)HDR_SIZE + (size_t)count * REC_SIZE;
    size_t names_end   = records_end + (size_t)count * 4;
    size_t idmap_end   = (size_t)idmap_off + ((size_t)max_id + 1) * 2;

    /* Every section must lie inside the blob, and they must appear in the
     * order the builder writes them. A truncated flash write fails here rather
     * than surfacing as garbage names much later. */
    if (records_end > size || names_end > size) {
        return false;
    }
    if (idmap_off != names_end || names_off < idmap_off) {
        return false;
    }
    if (max_id && idmap_end > size) {
        return false;
    }
    if (names_off > size) {
        return false;
    }

    db->base         = p;
    db->size         = size;
    db->count        = count;
    db->max_id       = max_id;
    db->records      = p + HDR_SIZE;
    db->name_offsets = (const uint32_t *)(const void *)(p + records_end);
    db->idmap        = max_id ? (const uint16_t *)(const void *)(p + idmap_off) : NULL;
    db->pool         = (const char *)(p + names_off);
    return true;
}

bool chipdb_get(const chipdb_t *db, uint16_t slot, chip_record_t *out) {
    if (!db->base || slot >= db->count || !out) {
        return false;
    }
    const uint8_t *r = db->records + (size_t)slot * REC_SIZE;
    out->index      = rd16(r + 0);
    out->element    = r[2];
    out->library    = r[3];
    out->mb         = r[4];
    out->gate_uses  = r[5];
    out->damage     = rd16(r + 6);
    out->lib_number = rd16(r + 8);
    out->rarity     = r[10];
    out->flags      = r[11];
    memcpy(out->codes, r + 12, 4);
    return true;
}

const char *chipdb_name(const chipdb_t *db, uint16_t slot) {
    if (!db->base || slot >= db->count) {
        return NULL;
    }
    uint32_t off = db->name_offsets[slot];
    const char *s = db->pool + off;
    if ((const uint8_t *)s >= db->base + db->size) {
        return NULL;
    }
    return s;
}

uint16_t chipdb_slot_for_id(const chipdb_t *db, uint16_t chip_id) {
    if (!db->base || !db->idmap || chip_id > db->max_id) {
        return CHIPDB_NO_RECORD;
    }
    uint16_t slot = db->idmap[chip_id];
    return slot < db->count ? slot : CHIPDB_NO_RECORD;
}

bool chipdb_get_by_id(const chipdb_t *db, uint16_t chip_id, chip_record_t *out) {
    uint16_t slot = chipdb_slot_for_id(db, chip_id);
    if (slot == CHIPDB_NO_RECORD) {
        return false;
    }
    return chipdb_get(db, slot, out);
}

char chipdb_code_char(uint8_t code) {
    if (code == CHIP_CODE_STAR) {
        return '*';
    }
    if (code < 26) {
        return (char)('A' + code);
    }
    return '\0';
}
