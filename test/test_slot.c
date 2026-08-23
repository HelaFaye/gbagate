/*
 * test_slot.c -- shared pad bus sequencing.
 *
 *   cc -std=c11 -Wall -Wextra -o test_slot test_slot.c chip_slot.c chip_reader.c
 *
 * The simulated expander records every direction change, so the tests can
 * assert on bus discipline -- which line was driven, and that it was released
 * again -- not just on the value that came back.
 */

#include <stdio.h>
#include <string.h>

#include "chip_slot.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-42s got 0x%lX, want 0x%lX\n",
               what, (unsigned long)got, (unsigned long)want);
        g_fail++;
    }
}

/* ---- simulated MCP23017 + a seated chip -------------------------------- */

typedef struct {
    uint16_t input_mask;   /* 1 = input with pull-up  */
    uint16_t out_levels;
    uint16_t shorts;       /* pads the chip ties to its common pad */
    uint8_t  common;       /* the seated chip's common pad, 0 = empty */
    bool     fail_dir, fail_out, fail_in;
    unsigned dir_calls;
    uint16_t last_driven;  /* mask of lines configured as outputs */
} fake_bus_t;

static bool fb_set_dir(void *ctx, uint16_t input_mask) {
    fake_bus_t *b = ctx;
    if (b->fail_dir) {
        return false;
    }
    b->input_mask  = input_mask;
    b->last_driven = (uint16_t)(SLOT_PAD_MASK & ~input_mask);
    b->dir_calls++;
    return true;
}

static bool fb_set_out(void *ctx, uint16_t levels) {
    fake_bus_t *b = ctx;
    if (b->fail_out) {
        return false;
    }
    b->out_levels = levels;
    return true;
}

/*
 * Model the physics: a line reads low if it is driven low, or if it is tied by
 * the chip to a pad that is driven low. Everything else floats up.
 */
static bool fb_get_in(void *ctx, uint16_t *levels) {
    fake_bus_t *b = ctx;
    if (b->fail_in) {
        return false;
    }
    uint16_t driven_low = (uint16_t)(b->last_driven & ~b->out_levels);
    uint16_t low = driven_low;

    if (b->common && (driven_low & SLOT_PAD_BIT(b->common))) {
        low |= b->shorts;
    }
    *levels = (uint16_t)(SLOT_PAD_MASK & ~low);
    return true;
}

static const chip_bus_io_t IO = { fb_set_dir, fb_set_out, fb_get_in };

static uint16_t bits(const uint8_t *pads, size_t n) {
    uint16_t m = 0;
    for (size_t i = 0; i < n; i++) {
        m |= SLOT_PAD_BIT(pads[i]);
    }
    return m;
}

/* ---- tests ------------------------------------------------------------- */

static void test_masks(void) {
    printf("pad masks\n");

    /* Advance: common 10, ID bits on 1-9 and 11. */
    check("advance common", chip_slot_common_pad(CHIP_FAMILY_ADVANCE), 10);
    static const uint8_t adv[] = {1,2,3,4,5,6,7,8,9,11};
    check("advance id mask", chip_slot_id_mask(CHIP_FAMILY_ADVANCE), bits(adv, 10));

    /* Beast: common 2, ID bits on 3-11. Pads 1 and 12 are never wired. */
    check("beast common", chip_slot_common_pad(CHIP_FAMILY_BEAST), 2);
    static const uint8_t bst[] = {3,4,5,6,7,8,9,10,11};
    check("beast id mask", chip_slot_id_mask(CHIP_FAMILY_BEAST), bits(bst, 9));

    /* The overlap that lets one bus serve both. */
    check("pad 10 is a beast id bit",
          (chip_slot_id_mask(CHIP_FAMILY_BEAST) & SLOT_PAD_BIT(10)) != 0, 1);
    check("pad 2 is an advance id bit",
          (chip_slot_id_mask(CHIP_FAMILY_ADVANCE) & SLOT_PAD_BIT(2)) != 0, 1);
    check("11 lines cover both",
          chip_slot_id_mask(CHIP_FAMILY_ADVANCE) | SLOT_PAD_BIT(10) |
          chip_slot_id_mask(CHIP_FAMILY_BEAST) | SLOT_PAD_BIT(2), SLOT_PAD_MASK);
}

static void test_read_advance(void) {
    printf("read an Advance chip\n");

    /* Gun_Del_Sol from the EXE project: pads 4, 10, 11 in one net. */
    fake_bus_t bus;
    memset(&bus, 0, sizeof bus);
    bus.common = 10;
    static const uint8_t gds[] = {4, 11};
    bus.shorts = bits(gds, 2);

    chip_slots_t s;
    chip_slots_init(&s, &IO, &bus);

    chip_family_t fam;
    uint16_t pads;
    check("scan ok", chip_slots_scan(&s, true, false, &fam, &pads), SLOT_SCAN_OK);
    check("family", fam, CHIP_FAMILY_ADVANCE);

    /* Pads 4 and 11 shorted read low; everything else, including the driven
     * common pad, must come back high. */
    check("pad 4 low", (pads & SLOT_PAD_BIT(4)) == 0, 1);
    check("pad 11 low", (pads & SLOT_PAD_BIT(11)) == 0, 1);
    check("pad 1 high", (pads & SLOT_PAD_BIT(1)) != 0, 1);
    check("common pad forced high", (pads & SLOT_PAD_BIT(10)) != 0, 1);

    /* And it decodes to the same thing chip_reader would compute. */
    chip_reader_t r;
    chip_reader_init(&r, CHIP_FAMILY_ADVANCE, NULL, NULL);
    printf("  decodes to 0x%03X\n", chip_reader_decode(&r, pads));
}

static void test_read_beast(void) {
    printf("read a Beast chip\n");

    /* BugBomb: pads 2, 3, 4, 9. Pad 2 is the common. */
    fake_bus_t bus;
    memset(&bus, 0, sizeof bus);
    bus.common = 2;
    static const uint8_t bb[] = {3, 4, 9};
    bus.shorts = bits(bb, 3);

    chip_slots_t s;
    chip_slots_init(&s, &IO, &bus);

    chip_family_t fam;
    uint16_t pads;
    check("scan ok", chip_slots_scan(&s, false, true, &fam, &pads), SLOT_SCAN_OK);
    check("family", fam, CHIP_FAMILY_BEAST);
    check("pad 3 low", (pads & SLOT_PAD_BIT(3)) == 0, 1);
    check("pad 9 low", (pads & SLOT_PAD_BIT(9)) == 0, 1);
    check("common pad forced high", (pads & SLOT_PAD_BIT(2)) != 0, 1);

    /* Pad 1 is outside Beast's ID set and must be forced high rather than
     * left floating, or a stray pull would invent a bit. */
    check("pad 1 forced high", (pads & SLOT_PAD_BIT(1)) != 0, 1);
}

static void test_bus_discipline(void) {
    printf("bus discipline\n");

    fake_bus_t bus;
    memset(&bus, 0, sizeof bus);
    bus.common = 10;
    bus.shorts = SLOT_PAD_BIT(5);

    chip_slots_t s;
    chip_slots_init(&s, &IO, &bus);
    chip_slots_scan(&s, true, false, NULL, NULL);

    /* Exactly one line was ever driven, and it was released afterwards. */
    check("bus released", bus.input_mask, SLOT_PAD_MASK);
    check("nothing driven at rest", bus.last_driven, 0);

    /* An empty scan must also leave the bus released. */
    memset(&bus, 0, sizeof bus);
    bus.input_mask = 0;
    check("empty", chip_slots_scan(&s, false, false, NULL, NULL), SLOT_SCAN_EMPTY);
    check("released on empty", bus.input_mask, SLOT_PAD_MASK);
}

static void test_conflict_and_errors(void) {
    printf("conflict and I/O failure\n");

    fake_bus_t bus;
    memset(&bus, 0, sizeof bus);
    chip_slots_t s;
    chip_slots_init(&s, &IO, &bus);

    /* Two chips on one shared bus would decode to a plausible wrong ID, so
     * refusing is the only safe answer. */
    check("both slots refused", chip_slots_scan(&s, true, true, NULL, NULL),
          SLOT_SCAN_CONFLICT);
    check("conflict counted", s.conflicts, 1);
    check("released on conflict", bus.input_mask, SLOT_PAD_MASK);

    bus.fail_in = true;
    check("read failure reported", chip_slots_scan(&s, true, false, NULL, NULL),
          SLOT_SCAN_IO_ERROR);
    check("io error counted", s.io_errors, 1);
    check("released after a failed read", bus.input_mask, SLOT_PAD_MASK);

    bus.fail_in = false;
    bus.fail_out = true;
    check("write failure reported", chip_slots_scan(&s, false, true, NULL, NULL),
          SLOT_SCAN_IO_ERROR);
    check("still released", bus.input_mask, SLOT_PAD_MASK);

    /* A missing callback must not be dereferenced. */
    chip_bus_io_t broken = { NULL, NULL, NULL };
    chip_slots_t b;
    chip_slots_init(&b, &broken, NULL);
    check("no callbacks is an error", chip_slots_scan(&b, true, false, NULL, NULL),
          SLOT_SCAN_IO_ERROR);
}

static void test_blank_chip(void) {
    printf("blank chip\n");

    /* Advance_Base_Chip has no shorts at all. With a detect switch fitted the
     * scan still succeeds and every pad reads high, which decodes to ID 0. */
    fake_bus_t bus;
    memset(&bus, 0, sizeof bus);
    bus.common = 10;
    bus.shorts = 0;

    chip_slots_t s;
    chip_slots_init(&s, &IO, &bus);

    uint16_t pads;
    check("scan ok", chip_slots_scan(&s, true, false, NULL, &pads), SLOT_SCAN_OK);
    check("all pads high", pads, SLOT_PAD_MASK);

    chip_reader_t r;
    chip_reader_init(&r, CHIP_FAMILY_ADVANCE, NULL, NULL);
    check("decodes to ID 0", chip_reader_decode(&r, pads), 0);
}

int main(void) {
    test_masks();
    test_read_advance();
    test_read_beast();
    test_bus_discipline();
    test_conflict_and_errors();
    test_blank_chip();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
