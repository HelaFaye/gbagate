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
    HW_CAP_USB         = 1u << 7,  /* USB is the drive (the "usb" setting)  */
    /* A link port with a source of transfers: the PHY on a board, the bridge
     * standing in for it under emulation. No cable is sensed -- the port has
     * no presence pin. */
    HW_CAP_LINK_PORT   = 1u << 8,
    HW_CAP_IR          = 1u << 9,  /* the IR emitter is fitted and came up  */
    HW_CAP_GB_5V       = 1u << 10, /* a 5 V level shifter on the link port:
                                      Game Boy partners (board.h)           */
    /* The link port's own pins, driven by this board: multiboot, IR, the GB
     * modes. Not under emulation, where HW_CAP_LINK_PORT is the UART bridge
     * standing in for the gate's transfers and nothing else. */
    HW_CAP_LINK_PINS   = 1u << 11,
    /* USB is the GB-Link adapter: one controller, so it is this or the drive
     * (HW_CAP_USB), never both. */
    HW_CAP_USB_GBLINK  = 1u << 12
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

/* As hw_caps_describe, with a '*' after each name in stand_in: a capability
 * that something under emulation provides instead of the hardware. */
size_t hw_caps_describe_marked(uint32_t mask, uint32_t stand_in, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HW_CAPS_H */
