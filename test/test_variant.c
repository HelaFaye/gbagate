/*
 * test_variant.c -- prove the three images are coherent before flashing one.
 *
 *   cc -std=c11 -Wall -Wextra -o test_variant test_variant.c variant.c
 */

#include <stdio.h>
#include <string.h>

#include "variant.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-46s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static bool has(uint32_t mask, feature_t f) { return (mask & f) != 0; }

static void test_link(void) {
    printf("image: link\n");

    variant_result_t r = variant_resolve(VARIANT_LINK);
    check("valid", r.status, VARIANT_VALID);

    check("parent role", has(r.resolved, FEAT_LINK_MASTER), 1);
    check("radio", has(r.resolved, FEAT_ESPNOW), 1);
    check("usb serial", has(r.resolved, FEAT_USB_CDC), 1);
    check("adapter emulation", has(r.resolved, FEAT_RFU_EMU), 1);

    /*
     * This used to assert the opposite: "a link cable is a parent; it never
     * has to answer someone else's clock", which kept this image small.
     *
     * GB-Link's relay changed that. When the netplay bridge or Celio hosts a
     * session the local GBA is the parent and the adapter answers as child,
     * and the GB-Link personality is in every image. The cost that justified
     * leaving the role out is also gone: both roles are one PHY engine now
     * (gba_link_esp32s3.c), so the child role adds no module.
     */
    check("GB-Link adapter", has(r.resolved, FEAT_GBLINK), 1);
    check("child role, for GB-Link's relay host side", has(r.resolved, FEAT_LINK_SLAVE), 1);
    check("no chip gate", has(r.resolved, FEAT_CHIPGATE), 0);
    check("no chip reader", has(r.resolved, FEAT_CHIP_READER), 0);
    check("no storage", has(r.resolved, FEAT_USB_MSC), 0);
}

static void test_gate(void) {
    printf("image: gate\n");

    variant_result_t r = variant_resolve(VARIANT_GATE);
    check("valid", r.status, VARIANT_VALID);

    /* The two deliberate omissions. */
    check("no physical chip reader", has(r.resolved, FEAT_CHIP_READER), 0);
    check("no infrared", has(r.resolved, FEAT_IR), 0);

    check("chip gate", has(r.resolved, FEAT_CHIPGATE), 1);
    check("child role, since a gate is one", has(r.resolved, FEAT_LINK_SLAVE), 1);
    check("all six databases", has(r.resolved, FEAT_CHIPDB_ALL), 1);
    check("multiboot", has(r.resolved, FEAT_MULTIBOOT), 1);
    check("usb storage", has(r.resolved, FEAT_USB_MSC), 1);
    check("adapter emulation", has(r.resolved, FEAT_RFU_EMU), 1);
}

static void test_full(void) {
    printf("image: full\n");

    variant_result_t full = variant_resolve(VARIANT_FULL);
    check("valid", full.status, VARIANT_VALID);
    check("chip reader", has(full.resolved, FEAT_CHIP_READER), 1);
    check("infrared", has(full.resolved, FEAT_IR), 1);
    check("nRF24", has(full.resolved, FEAT_NRF24), 1);

    /* The tiers must nest, or moving up one silently loses a feature. */
    variant_result_t gate = variant_resolve(VARIANT_GATE);
    variant_result_t link = variant_resolve(VARIANT_LINK);
    /* mini is not a subset of anything: it trades USB storage for SD, so it
     * has a feature the others lack and lacks several they have. */
    variant_result_t mini = variant_resolve(VARIANT_MINI);
    check("mini has SD storage", (mini.resolved & FEAT_SD_STORAGE) != 0, 1);
    check("and no USB storage", (mini.resolved & FEAT_USB_MSC) != 0, 0);
    check("but can still multiboot", (mini.resolved & FEAT_MULTIBOOT) != 0, 1);

    check("full contains gate", gate.resolved & ~full.resolved, 0);
    check("full is strictly larger", full.resolved != gate.resolved, 1);
    check("gate contains link", link.resolved & ~gate.resolved, 0);
    check("gate is strictly larger", gate.resolved != link.resolved, 1);
}

static void test_dependencies(void) {
    printf("dependency rules\n");

    /* Every feature every image enables must have its prerequisites. This is
     * the check that catches a table edit made in a hurry. */
    for (int id = 0; id < VARIANT_COUNT; id++) {
        variant_result_t r = variant_resolve((variant_id_t)id);
        const variant_t *v = variant_get((variant_id_t)id);
        if (r.status != VARIANT_VALID) {
            printf("  FAIL %s: %s (%s needs something absent)\n",
                   v->name, variant_status_name(r.status), feature_name(r.offender));
            g_fail++;
            continue;
        }
        for (int i = 0; i < FEATURE_COUNT; i++) {
            feature_t f = (feature_t)(1u << i);
            if (!(r.resolved & f)) continue;
            if (feature_requires(f) & ~r.resolved) {
                printf("  FAIL %s: %s missing a prerequisite\n",
                       v->name, feature_name(f));
                g_fail++;
            }
        }
    }
    check("all images coherent", g_fail, 0);

    /* The relationships the split turns on, asserted rather than assumed. */
    check("a gate is a child",
          feature_requires(FEAT_CHIPGATE) & FEAT_LINK_SLAVE, FEAT_LINK_SLAVE);
    check("a gate needs a screen",
          feature_requires(FEAT_CHIPGATE) & FEAT_DISPLAY, FEAT_DISPLAY);
    check("multiboot is a parent role",
          feature_requires(FEAT_MULTIBOOT) & FEAT_LINK_MASTER, FEAT_LINK_MASTER);
    /*
     * Multiboot needs storage, but either kind will do -- USB on the full
     * boards, SD on the mini. A requires-mask cannot say "one of these two",
     * so the rule lives in variant_resolve() and is checked by result.
     */
    check("multiboot is a parent role",
          feature_requires(FEAT_MULTIBOOT) & FEAT_LINK_MASTER, FEAT_LINK_MASTER);
    for (int id = 0; id < VARIANT_COUNT; id++) {
        variant_result_t r = variant_resolve((variant_id_t)id);
        if (r.status != VARIANT_VALID || !(r.resolved & FEAT_MULTIBOOT)) {
            continue;
        }
        if (!(r.resolved & (FEAT_USB_MSC | FEAT_SD_STORAGE))) {
            printf("  FAIL %s has multiboot and no storage\n",
                   variant_get((variant_id_t)id)->name);
            g_fail++;
        }
    }
    check("every multiboot image has storage of some kind", g_fail, 0);
    check("a chip reader needs a gate to send to",
          feature_requires(FEAT_CHIP_READER) & FEAT_CHIPGATE, FEAT_CHIPGATE);
    check("adapter emulation carries payload over the radio",
          feature_requires(FEAT_RFU_EMU) & FEAT_ESPNOW, FEAT_ESPNOW);

    check("unknown variant rejected",
          variant_resolve((variant_id_t)99).status, VARIANT_ERR_UNKNOWN);
}

static void test_optional(void) {
    printf("optional hardware\n");

    /*
     * The nRF24 is compiled in and gated at boot by hw_caps, so a board with
     * an unpopulated footprint runs the same image. It must not be a separate
     * build, and it must not make the image invalid.
     */
    const variant_t *full = variant_get(VARIANT_FULL);
    check("nRF24 marked optional", (full->optional & FEAT_NRF24) != 0, 1);
    check("still valid regardless", variant_resolve(VARIANT_FULL).status, VARIANT_VALID);

    /* Nothing may depend on something optional, or an unpopulated board would
     * break a feature that appears mandatory. */
    for (int id = 0; id < VARIANT_COUNT; id++) {
        const variant_t *v = variant_get((variant_id_t)id);
        for (int i = 0; i < FEATURE_COUNT; i++) {
            feature_t f = (feature_t)(1u << i);
            if (!(v->features & f) || (v->optional & f)) continue;
            if (feature_requires(f) & v->optional) {
                printf("  FAIL %s: required %s depends on optional hardware\n",
                       v->name, feature_name(f));
                g_fail++;
            }
        }
    }
}

static void report(void) {
    printf("\nimages\n");
    char buf[420];
    for (int id = 0; id < VARIANT_COUNT; id++) {
        const variant_t *v = variant_get((variant_id_t)id);
        variant_result_t r = variant_resolve((variant_id_t)id);
        variant_describe(r.resolved, buf, sizeof buf);
        printf("  %-5s %s\n         %s\n", v->name, v->summary, buf);
    }
}

int main(void) {
    test_link();
    test_gate();
    test_full();
    test_dependencies();
    test_optional();
    report();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
