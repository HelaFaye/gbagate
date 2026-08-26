/*
 * test_chipdb.c -- validate the packed database against the real blobs.
 *
 *   cc -std=c11 -Wall -Wextra -o test_chipdb test_chipdb.c chipdb.c
 *   ./test_chipdb chipdb/bn6g.chdb
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chipdb.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-38s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static void check_str(const char *what, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        printf("  FAIL %-38s got \"%s\", want \"%s\"\n",
               what, got ? got : "(null)", want);
        g_fail++;
    }
}

static void *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "chipdb/bn6g.chdb";
    size_t size = 0;
    void *blob = slurp(path, &size);
    if (!blob) {
        printf("cannot read %s\n", path);
        return 2;
    }

    chipdb_t db;
    printf("open %s (%zu bytes)\n", path, size);
    check("opens", chipdb_open(&db, blob, size), 1);

    /* The detailed expectations below are BN6 Gregar's. Every other blob still
     * gets the structural checks, which is what actually guards the format. */
    bool is_bn6g = strcmp(chipdb_game(&db), "bn6g") == 0;
    printf("game %s, %u records%s\n", chipdb_game(&db), chipdb_count(&db),
           is_bn6g ? "" : " (generic checks only)");
    if (is_bn6g) {
        check("record count", chipdb_count(&db), 411);
    }

    /* Wire IDs are what the reader produces and what slot 5 carries. */
    printf("wire ID lookup\n");
    chip_record_t rec;
    check("id 1 resolves", chipdb_get_by_id(&db, 1, &rec), 1);
    if (is_bn6g) {
    check_str("id 1 name", chipdb_name(&db, chipdb_slot_for_id(&db, 1)), "Cannon");
    check("id 1 mb", rec.mb, 6);
    check("id 1 gate uses", rec.gate_uses, 3);
    check("id 1 is standard", rec.library, CHIP_LIB_STANDARD);
    check("id 1 code A", chipdb_code_char(rec.codes[0]), 'A');
    check("id 1 code *", chipdb_code_char(rec.codes[3]), '*');

    check("id 2 resolves", chipdb_get_by_id(&db, 2, &rec), 1);
    check_str("id 2 name", chipdb_name(&db, chipdb_slot_for_id(&db, 2)), "HiCannon");
    check_str("id 3 name", chipdb_name(&db, chipdb_slot_for_id(&db, 3)), "M-Cannon");
    check_str("id 4 name", chipdb_name(&db, chipdb_slot_for_id(&db, 4)), "AirShot");
    }

    /* Unmapped and out-of-range IDs must fail cleanly, not read garbage --
     * the reader will hand us plenty of both. */
    printf("unmapped handling\n");
    check("id 0 unmapped", chipdb_slot_for_id(&db, 0), CHIPDB_NO_RECORD);
    if (is_bn6g) {
        check("id 204 unmapped", chipdb_slot_for_id(&db, 204), CHIPDB_NO_RECORD);
    }
    check("id 60000 out of range", chipdb_slot_for_id(&db, 60000), CHIPDB_NO_RECORD);
    check("get_by_id fails cleanly", chipdb_get_by_id(&db, 60000, &rec), 0);

    /* Every mapped ID must land on a real record with a readable name. */
    printf("map integrity\n");
    unsigned mapped = 0;
    for (uint32_t id = 0; id <= db.max_id; id++) {
        uint16_t slot = chipdb_slot_for_id(&db, (uint16_t)id);
        if (slot == CHIPDB_NO_RECORD) {
            continue;
        }
        mapped++;
        if (slot >= chipdb_count(&db)) {
            check("slot in range", 0, 1);
            break;
        }
        const char *name = chipdb_name(&db, slot);
        if (!name || !name[0] || strlen(name) > 12) {
            printf("  FAIL id %u -> slot %u has bad name\n", id, slot);
            g_fail++;
            break;
        }
    }
    printf("  %u of %u IDs mapped\n", mapped, db.max_id);
    if (is_bn6g) {
        check("mapped count matches builder", mapped, 285);
    }
    check("something mapped", mapped > 200, 1);

    /* The inverse map -- the number the gate actually puts in slot 5. Every
     * mapped ID must round-trip. That is the correctness check for
     * chipdb_id_for_slot(), and because each ID lands on a distinct slot it is
     * also a proof that the builder's map is injective, which is what makes the
     * inverse well defined in the first place. */
    printf("wire ID round trip\n");
    unsigned round_tripped = 0, diverged = 0;
    for (uint32_t id = 0; id <= db.max_id; id++) {
        uint16_t slot = chipdb_slot_for_id(&db, (uint16_t)id);
        if (slot == CHIPDB_NO_RECORD) {
            continue;
        }
        uint16_t back = chipdb_id_for_slot(&db, slot);
        if (back != id) {
            printf("  FAIL id %u -> slot %u -> id %u\n", id, slot, back);
            g_fail++;
            break;
        }
        round_tripped++;
        if (chipdb_get(&db, slot, &rec) && rec.index != id) {
            diverged++;   /* what the old gate_app.c would have sent instead */
        }
    }
    check("every mapped ID round trips", round_tripped, mapped);
    printf("  %u of %u would have gone out as the wrong chip via rec.index\n",
           diverged, round_tripped);

    /* A record no physical chip can address must say so rather than invent an
     * ID -- refusing is correct, sending a plausible wrong number is not. */
    unsigned no_id = 0;
    for (uint16_t s = 0; s < chipdb_count(&db); s++) {
        if (chipdb_id_for_slot(&db, s) == CHIPDB_NO_ID) {
            no_id++;
        }
    }
    printf("  %u of %u records have no wire ID and cannot be sent\n",
           no_id, chipdb_count(&db));
    check("round tripped + unsendable == records",
          round_tripped + no_id, chipdb_count(&db));
    check("out of range slot has no ID",
          chipdb_id_for_slot(&db, 60000), CHIPDB_NO_ID);
    if (is_bn6g) {
        /* Cross-checks against tools/check_wire_ids.py: it reports bn6g as
         * 285 mapped of 411 records with 2 diverging. */
        check("bn6g records with no wire ID", no_id, 411 - 285);
        check("bn6g IDs rec.index would misname", diverged, 2);
    }

    /* Every record must be readable and self-consistent. */
    for (uint16_t s = 0; s < chipdb_count(&db); s++) {
        if (!chipdb_get(&db, s, &rec) || !chipdb_name(&db, s)) {
            check("record readable", 0, 1);
            break;
        }
        if (rec.gate_uses > 3 || rec.library > CHIP_LIB_PA) {
            printf("  FAIL slot %u implausible: uses=%u lib=%u\n",
                   s, rec.gate_uses, rec.library);
            g_fail++;
            break;
        }
    }
    check("out of range slot", chipdb_get(&db, 60000, &rec), 0);

    /* Truncation and corruption must be rejected at open, not later. */
    printf("blob validation\n");
    chipdb_t bad;
    check("truncated rejected", chipdb_open(&bad, blob, size / 2), 0);
    check("tiny rejected", chipdb_open(&bad, blob, 8), 0);
    check("null rejected", chipdb_open(&bad, NULL, size), 0);
    unsigned char *corrupt = malloc(size);
    memcpy(corrupt, blob, size);
    corrupt[0] = 'X';
    check("bad magic rejected", chipdb_open(&bad, corrupt, size), 0);
    memcpy(corrupt, blob, size);
    corrupt[4] = 99;
    check("bad version rejected", chipdb_open(&bad, corrupt, size), 0);
    free(corrupt);

    printf("\nsample: what the ST7735 row would show\n");
    const uint16_t show[] = { 1, 2, 5, 50, 100, 200, 290 };
    for (unsigned i = 0; i < sizeof show / sizeof show[0]; i++) {
        uint16_t slot = chipdb_slot_for_id(&db, show[i]);
        if (slot == CHIPDB_NO_RECORD) {
            continue;
        }
        chipdb_get(&db, slot, &rec);
        char codes[5] = {0};
        int c = 0;
        for (int k = 0; k < 4; k++) {
            char ch = chipdb_code_char(rec.codes[k]);
            if (ch) {
                codes[c++] = ch;
            }
        }
        printf("  id %3u  %-9s %-4s %2u MB  dmg %3u  uses %u\n",
               show[i], chipdb_name(&db, slot), codes, rec.mb,
               rec.damage, rec.gate_uses);
    }

    free(blob);
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
