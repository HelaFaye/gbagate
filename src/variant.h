/*
 * variant.h -- build variants, as data rather than as scattered #ifdefs.
 *
 * Three firmware images are wanted, and they differ by more than a few flags:
 * one runs on a different silicon family, one deliberately omits hardware the
 * others assume. Expressed as #ifdefs that becomes unauditable -- the question
 * "does this combination actually work" has no place to live.
 *
 * So features, platforms and their dependencies are a table, checked by
 * variant_validate() on a host before anything is flashed. A combination that
 * cannot work fails a test rather than a bench session.
 *
 * Target
 * ------
 * ESP32-S3 only. An earlier revision also targeted the S2 and carried a
 * platform dimension with capability pruning to go with it; dropping the S2
 * removed the only thing that dimension ever expressed, so it is gone rather
 * than left as scaffolding with nothing to hold up.
 *
 * The reason the S2 went is worth keeping written down, because it is the
 * constraint the whole architecture rests on. The link roles divide by who
 * controls the clock:
 *
 *   parent (master): we choose when each transfer starts. A stall stretches a
 *                    gap and the GBA waits.
 *   child (slave):   our word must be staged before the parent clocks it. A
 *                    stall corrupts the frame outright.
 *
 * The child role therefore needs a core that WiFi and USB cannot steal, and a
 * Battle Chip Gate is a child by definition. On a single-core part the gate is
 * not a reduced feature, it is an impossible one.
 */

#ifndef VARIANT_H
#define VARIANT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- features --------------------------------------------------------- */

typedef enum {
    FEAT_DISPLAY      = 1u << 0,  /* ST7735 and the renderer            */
    FEAT_ENCODER      = 1u << 1,  /* rotary encoder and button          */
    FEAT_CHIPGATE     = 1u << 2,  /* Battle Chip Gate responder         */
    FEAT_CHIP_READER  = 1u << 3,  /* physical chip slots via MCP23017   */
    FEAT_CHIPDB_ALL   = 1u << 4,  /* all six databases, not just one    */
    FEAT_LINK_MASTER  = 1u << 5,  /* Multi-Play parent role             */
    FEAT_LINK_SLAVE   = 1u << 6,  /* Multi-Play child role              */
    FEAT_ESPNOW       = 1u << 7,  /* board-to-board radio               */
    FEAT_USB_CDC      = 1u << 8,  /* bridge to an emulator              */
    FEAT_USB_MSC      = 1u << 9,  /* drag-and-drop storage              */
    FEAT_MULTIBOOT    = 1u << 10, /* send a program to a bare GBA       */
    FEAT_RFU_EMU      = 1u << 11, /* emulate the Wireless Adapter       */
    FEAT_IR           = 1u << 12, /* infrared accessory protocol        */
    FEAT_NRF24        = 1u << 13, /* optional second radio              */
    FEAT_SELFTEST     = 1u << 14, /* built-in link self-test            */
    FEAT_USB_EXPOSE  = 1u << 15  /* expose the drive to a host         */
} feature_t;

#define FEATURE_COUNT 16

/* ---- variants --------------------------------------------------------- */

typedef enum {
    VARIANT_LINK = 0,  /* wireless link cable / adapter. S2 or S3.        */
    VARIANT_GATE,      /* chip gate and everything but chips and IR. S3.  */
    VARIANT_FULL,      /* everything. S3.                                 */
    VARIANT_COUNT
} variant_id_t;

typedef struct {
    const char *name;
    uint32_t    features;
    /*
     * Features whose hardware may or may not be fitted. They are compiled in
     * but gated at runtime by hw_caps, so an unpopulated footprint is a normal
     * board rather than a different build.
     */
    uint32_t    optional;
    const char *summary;
} variant_t;

const variant_t *variant_get(variant_id_t id);
const char      *feature_name(feature_t f);

/* Features a feature depends on. 0 if none. */
uint32_t feature_requires(feature_t f);

typedef enum {
    VARIANT_VALID = 0,
    VARIANT_ERR_UNKNOWN,      /* no such variant                     */
    VARIANT_ERR_DEPENDENCY    /* a feature's prerequisite is missing */
} variant_status_t;

typedef struct {
    variant_status_t status;
    uint32_t         resolved;  /* features this image enables    */
    feature_t        offender;  /* the feature that failed, if any */
    uint32_t         missing;   /* what it needed and did not get   */
} variant_result_t;

/* Check that every feature in an image has its prerequisites. */
variant_result_t variant_resolve(variant_id_t id);

const char *variant_status_name(variant_status_t s);

/* Render a feature mask as "display, encoder, chip gate" into buf. */
size_t variant_describe(uint32_t features, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* VARIANT_H */
