/*
 * variant.c -- see variant.h
 */

#include <string.h>

#include "variant.h"

/* ---- feature rules ----------------------------------------------------- */

uint32_t feature_requires(feature_t f) {
    switch (f) {
    /* A gate is a Multi-Play child, and it needs a screen to pick chips on. */
    case FEAT_CHIPGATE:    return FEAT_LINK_SLAVE | FEAT_DISPLAY | FEAT_ENCODER;

    /* Reading a chip is pointless without somewhere to send it and something
     * to show. */
    case FEAT_CHIP_READER: return FEAT_CHIPGATE | FEAT_DISPLAY;

    case FEAT_CHIPDB_ALL:  return FEAT_CHIPGATE;

    /* Self-test drives the link and reports on screen. */
    case FEAT_SELFTEST:    return FEAT_DISPLAY | FEAT_LINK_MASTER;

    /* Exposing the drive needs a drive and a screen to say so on. */
    case FEAT_USB_EXPOSE: return FEAT_DISPLAY | FEAT_USB_MSC;
    case FEAT_SD_STORAGE: return 0;

    /*
     * Infrared uses the link connector in General Purpose mode rather than the
     * Multi-Play machinery -- a different mode of the same pins.
     */
    case FEAT_IR:         return FEAT_DISPLAY | FEAT_LINK_MASTER;

    /* Something has to drive the encoder's selection. */
    case FEAT_ENCODER:     return FEAT_DISPLAY;

    /* Multiboot is a parent role, and the programs have to come from
     * somewhere the user can put them. */
    /*
     * Multiboot needs a parent role and somewhere the programs live. Either
     * storage will do, so this is checked in variant_resolve() rather than
     * expressed here -- a plain requires-mask cannot say "one of these two".
     */
    case FEAT_MULTIBOOT:   return FEAT_LINK_MASTER;

    /* The adapter presents itself to the GBA over the link port and carries
     * payload over the radio. */
    case FEAT_RFU_EMU:     return FEAT_LINK_MASTER | FEAT_ESPNOW;

    default:               return 0;
    }
}

/* ---- the three images -------------------------------------------------- */

static const variant_t k_variant[VARIANT_COUNT] = {
    [VARIANT_LINK] = {
        .name = "link",
        .features = FEAT_DISPLAY | FEAT_ENCODER |
                    FEAT_LINK_MASTER | FEAT_ESPNOW | FEAT_USB_CDC |
                    FEAT_RFU_EMU | FEAT_SELFTEST,
        .optional = 0,
        .summary = "wireless link cable and Wireless Adapter emulation",
    },
    [VARIANT_GATE] = {
        .name = "gate",
        .features = FEAT_DISPLAY | FEAT_ENCODER | FEAT_CHIPGATE |
                    FEAT_CHIPDB_ALL | FEAT_LINK_MASTER | FEAT_LINK_SLAVE |
                    FEAT_ESPNOW | FEAT_USB_CDC | FEAT_USB_MSC |
                    FEAT_MULTIBOOT | FEAT_RFU_EMU | FEAT_SELFTEST |
                    FEAT_USB_EXPOSE,
        .optional = 0,
        .summary = "everything except physical chips and IR",
    },
    [VARIANT_MINI] = {
        .name = "mini",
        .features = FEAT_DISPLAY | FEAT_ENCODER | FEAT_CHIPGATE |
                    FEAT_CHIPDB_ALL | FEAT_LINK_MASTER | FEAT_LINK_SLAVE |
                    FEAT_MULTIBOOT | FEAT_SD_STORAGE | FEAT_SELFTEST,
        .optional = 0,
        .summary = "gate, link and multiboot, from an SD card",
    },
    [VARIANT_FULL] = {
        .name = "full",
        .features = FEAT_DISPLAY | FEAT_ENCODER | FEAT_CHIPGATE |
                    FEAT_CHIP_READER | FEAT_CHIPDB_ALL |
                    FEAT_LINK_MASTER | FEAT_LINK_SLAVE |
                    FEAT_ESPNOW | FEAT_USB_CDC | FEAT_USB_MSC |
                    FEAT_MULTIBOOT | FEAT_RFU_EMU | FEAT_IR | FEAT_NRF24 |
                    FEAT_SELFTEST | FEAT_USB_EXPOSE,
        /* The nRF24 may or may not be populated; hw_caps decides at boot and
         * the menu entry appears or does not. Same image either way. */
        .optional = FEAT_NRF24,
        .summary = "every feature",
    },
};

const variant_t *variant_get(variant_id_t id) {
    return (id < VARIANT_COUNT) ? &k_variant[id] : NULL;
}

/* ---- resolution -------------------------------------------------------- */

variant_result_t variant_resolve(variant_id_t id) {
    variant_result_t r;
    memset(&r, 0, sizeof r);

    const variant_t *v = variant_get(id);
    if (!v) {
        r.status = VARIANT_ERR_UNKNOWN;
        return r;
    }

    r.resolved = v->features;

    for (int i = 0; i < FEATURE_COUNT; i++) {
        feature_t f = (feature_t)(1u << i);
        if (!(r.resolved & f)) {
            continue;
        }
        if (f == FEAT_MULTIBOOT &&
            !(r.resolved & (FEAT_USB_MSC | FEAT_SD_STORAGE))) {
            /* Programs have to come from somewhere. Either backend will do,
             * which a plain requires-mask cannot express. */
            r.status   = VARIANT_ERR_DEPENDENCY;
            r.offender = f;
            r.missing  = FEAT_SD_STORAGE;
            return r;
        }
        uint32_t missing = feature_requires(f) & ~r.resolved;
        if (missing) {
            r.status   = VARIANT_ERR_DEPENDENCY;
            r.offender = f;
            r.missing  = missing;
            return r;
        }
    }

    r.status = VARIANT_VALID;
    return r;
}

/* ---- names -------------------------------------------------------------- */

const char *feature_name(feature_t f) {
    switch (f) {
    case FEAT_DISPLAY:     return "display";
    case FEAT_ENCODER:     return "encoder";
    case FEAT_CHIPGATE:    return "chip gate";
    case FEAT_CHIP_READER: return "chip reader";
    case FEAT_CHIPDB_ALL:  return "all databases";
    case FEAT_LINK_MASTER: return "link parent";
    case FEAT_LINK_SLAVE:  return "link child";
    case FEAT_ESPNOW:      return "ESP-NOW";
    case FEAT_USB_CDC:     return "USB serial";
    case FEAT_USB_MSC:     return "USB storage";
    case FEAT_MULTIBOOT:   return "multiboot";
    case FEAT_RFU_EMU:     return "adapter emulation";
    case FEAT_IR:          return "infrared";
    case FEAT_NRF24:       return "nRF24";
    case FEAT_SELFTEST:    return "self test";
    case FEAT_USB_EXPOSE:  return "expose drive";
    case FEAT_SD_STORAGE:  return "SD card";
    default:               return "?";
    }
}

const char *variant_status_name(variant_status_t s) {
    switch (s) {
    case VARIANT_VALID:          return "valid";
    case VARIANT_ERR_UNKNOWN:    return "no such variant";
    case VARIANT_ERR_DEPENDENCY: return "missing dependency";
    default:                     return "?";
    }
}

size_t variant_describe(uint32_t features, char *buf, size_t cap) {
    if (cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    size_t used = 0;
    bool first = true;

    for (int i = 0; i < FEATURE_COUNT; i++) {
        feature_t f = (feature_t)(1u << i);
        if (!(features & f)) {
            continue;
        }
        const char *n = feature_name(f);
        size_t need = strlen(n) + (first ? 0u : 2u);
        if (used + need + 1 > cap) {
            break;
        }
        if (!first) {
            memcpy(buf + used, ", ", 2);
            used += 2;
        }
        memcpy(buf + used, n, strlen(n));
        used += strlen(n);
        buf[used] = '\0';
        first = false;
    }
    return used;
}
