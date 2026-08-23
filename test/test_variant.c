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
     * A link cable is a parent; it never has to answer someone else's clock.
     * Keeping the child role out is what makes this image the small one.
     */
    check("no child role", has(r.resolved, FEAT_LINK_SLAVE), 0);
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
    check("multiboot needs somewhere to keep programs",
          feature_requires(FEAT_MULTIBOOT) & FEAT_USB_MSC, FEAT_USB_MSC);
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
