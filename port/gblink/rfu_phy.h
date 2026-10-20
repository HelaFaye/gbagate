/*
 * rfu_phy.h -- the narrow driver under GB-Link's wireless adapter link layer.
 *
 * rfu_link_core.c implements the vendored layers/linkLayer_rfu.h -- the API
 * the wireless adapter section (sections/rfuProtocolSection.cpp) is written
 * against -- once, in portable C: the transfer-done callback, the role and
 * enable state, and upstream's transfer-path timing statistics (command 0x4C).
 * A board supplies the wire, and this is that interface. Upstream's RP2040
 * does it with two PIO programs (linkLayer_rfu.c); what they do is the
 * contract here:
 *
 * GBA_MASTER role (the GBA clocks; 32-bit Normal mode, MSB first, 256 kHz
 * during the ID exchange and 2 MHz after). SC idles high; the GBA shifts on
 * the falling edge and samples on the rising one. Per word the driver
 *   - drives the staged word on SI while reading SO,
 *   - then holds SI low ("not ready") until stage() supplies the next word,
 *   - and from then until SC next falls mirrors SI = NOT SO. librfu's master
 *     handshake (handshake_wait(1), raise SO, handshake_wait(0), start) is
 *     what that mirror answers; a GBA that finds SI low simply waits, so a
 *     late stage costs time, not data.
 *   - gbl_rfu_word_done(rx) is called once per word, after the last bit.
 * The ID exchange is the exception: librfu's Sio32ID clocks the next word
 * after a fixed ~40 us delay with no handshake, so the answer must be staged
 * within that.
 *
 * ADAPTER_MASTER role (the adapter clocks, ~150 kHz): exchange() clocks one
 * word synchronously; drive_si() sets SI between words. librfu's slave
 * handshake lives above, in the section.
 *
 * set_role() must hand SC over without an edge: an armed GBA-as-slave counts
 * every SC edge as a data bit.
 */
#ifndef GBAGATE_RFU_PHY_H
#define GBAGATE_RFU_PHY_H

#include <stdbool.h>
#include <stdint.h>

#include "layers/linkLayer_rfu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Take the link pins, GBA_MASTER role, nothing staged. */
    void (*enable)(void);
    void (*disable)(void);
    /* Swap roles glitch-free; drops anything staged. */
    void (*set_role)(enum RfuRole role);
    /* GBA_MASTER: the word to answer the GBA's next one with. */
    void (*stage)(uint32_t word);
    /* ADAPTER_MASTER: clock one word out, return the GBA's. False on timeout. */
    bool (*exchange)(uint32_t word, uint32_t *rx, uint32_t timeout_us);
    /* ADAPTER_MASTER, between words: SI level. */
    void (*drive_si)(bool high);
    /* The GBA's SO line. */
    bool (*so_high)(void);
    /* GBA_MASTER: start the bit counter over (a slipped word boundary), nothing
     * staged. */
    void (*realign)(void);
    /* The game's AgbRFU_SoftReset pulse: true once per pulse, which must have
     * held SD high for RFU_SD_HOLD_US without falling; *pin = which SD path. */
    bool (*sd_reset_seen)(uint8_t *pin);
    /* After sd_reset_seen: return once SD is low again and the lines settled. */
    void (*wait_reset_end)(void);
    /* bit0 GBA SO, bit1 our SI, bit2 SC (for the stall snapshot). */
    uint8_t (*lines)(void);
} gbl_rfu_phy_t;

/* Upstream's qualification of the reset pulse (linkLayer_rfu.c). */
#define RFU_SD_HOLD_US      40u
#define RFU_SD_SETTLE_US    60u
#define RFU_SD_PULSE_MAX_US 3000u
/* A transfer-done pass longer than this counts as late (upstream's figure). */
#define RFU_ISR_BUDGET_US   30u
#define RFU_PIN_NONE        0xFFu

/* Install the board's driver. Call once before gblink_start(). */
void gbl_rfu_install(const gbl_rfu_phy_t *drv);

/* ---- called by the driver ------------------------------------------------ */

/* GBA_MASTER: a word arrived. Runs the section's callback, which stages the
 * answer to the next one through stage(). Takes the shim lock. */
void gbl_rfu_word_done(uint32_t rx);

#ifdef __cplusplus
}
#endif

#endif /* GBAGATE_RFU_PHY_H */
