/*
 * hw_caps.h -- boot-time hardware detection and capability-gated menus.
 *
 * Everything optional on the board gets probed once at startup and turned into
 * a bit in a capability mask. Menu entries declare which bits they need, and
 * anything unsatisfied is not merely greyed out -- it never appears.
 *
 * One deliberate exception to that rule: hw_menu_hidden() exists so a
 * diagnostics screen can list what was suppressed and why. Without it, "the
 * wireless option is missing" is an unfalsifiable bug report, and the first
 * person to hit it will be you at 1am with a cold solder joint on CSN.
 */

#ifndef HW_CAPS_H
#define HW_CAPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HW_CAP_DISPLAY     = 1u << 0,  /* ST7735 responded to a register read   */
    HW_CAP_CHIP_READER = 1u << 1,  /* edge connector present                */
    HW_CAP_GPIO_EXP    = 1u << 2,  /* MCP23017 for the reader pads          */
    HW_CAP_NRF24       = 1u << 3,  /* an nRF24-family radio answered        */
    HW_CAP_NRF24_PLUS  = 1u << 4,  /* ...and it is a genuine nRF24L01+      */
    HW_CAP_WIFI        = 1u << 5,
    HW_CAP_BLE         = 1u << 6,
    HW_CAP_USB         = 1u << 7,  /* USB cable enumerated                  */
    HW_CAP_LINK_PORT   = 1u << 8   /* GBA link cable detected on SD/SC      */
} hw_cap_t;

typedef struct {
    int         id;
    const char *label;
    uint32_t    requires;  /* every bit must be set for the entry to show */
    const char *absent_hint; /* diagnostics only; never shown in the menu */
} menu_entry_t;

/* Collect visible entries into out[], preserving order.
 * Returns how many were written. */
size_t hw_menu_filter(const menu_entry_t *all, size_t count, uint32_t caps,
                      const menu_entry_t **out, size_t out_cap);

/* The complement, for a diagnostics screen. */
size_t hw_menu_hidden(const menu_entry_t *all, size_t count, uint32_t caps,
                      const menu_entry_t **out, size_t out_cap);

/* Which required bits are missing for this entry. 0 means it is visible. */
uint32_t hw_menu_missing(const menu_entry_t *entry, uint32_t caps);

/* Human-readable name for a single capability bit. */
const char *hw_cap_name(hw_cap_t cap);

/* Render a missing-capability mask as "nRF24, GPIO expander" into buf. */
size_t hw_caps_describe(uint32_t mask, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HW_CAPS_H */
