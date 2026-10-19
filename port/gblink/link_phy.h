/*
 * link_phy.h -- the narrow driver under GB-Link's link layer.
 *
 * link_layer_core.c implements every function in the vendored layers/linkLayer.h
 * -- callbacks, cable detection, the SD-pin override and flip, the wrong-pin
 * recovery budget, the e-Reader and partner-presence pin states -- once, in
 * portable C. What is left for a board is moving bits, and that is this
 * interface: the ESP32-S3 PHY (port/esp32s3/gblink_phy_esp32s3.c) and the host
 * simulator (test/gblink_sim/sim_phy.c) each provide one.
 *
 * The word cycle, as the RP2040 PIO programs run it and as a driver must:
 *
 *   MASTER  gbl_phy_next_tx() -> {word, gap}; drive the word; read the child's
 *           reply; gbl_phy_word_done(rx); wait gap; repeat.
 *   SLAVE   the GBA drops SC; gbl_phy_next_tx() -> word to answer with;
 *           read the GBA's word; drive ours; gbl_phy_word_done(rx).
 *
 * In SLAVE the answer must exist before the GBA's word has finished arriving,
 * so a driver that cannot call back in time (an interrupt that must not take
 * locks) may ask for it early -- right after the previous word_done -- since
 * nothing the vendored code does between those two points changes it.
 *
 * Timing values are upstream's units: PIO delay-loop iterations at clkdiv
 * 67.816, i.e. GBL_PHY_LOOP_NS each. Keeping them unconverted means every
 * constant in the vendored code means what it says.
 */
#ifndef GBAGATE_LINK_PHY_H
#define GBAGATE_LINK_PHY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 / (125 MHz / 67.816) = 542.528 ns, upstream's PIO instruction time. */
#define GBL_PHY_LOOP_PS 542528u

static inline uint64_t gbl_phy_loops_to_ns(uint32_t loops) {
    return ((uint64_t)loops * GBL_PHY_LOOP_PS) / 1000u;
}

typedef enum {
    GBL_PHY_OFF = 0,
    GBL_PHY_MASTER,        /* we drive SC                                   */
    GBL_PHY_SLAVE,         /* the GBA drives SC; we answer in slot 1        */
    GBL_PHY_SLAVE_EREADER, /* as SLAVE, but SO held low and never driven    */
    GBL_PHY_PRESENCE       /* GPIO only: SC/SI pulled-up inputs, SO low,
                              both SD paths driven high                      */
} gbl_phy_mode_t;

typedef struct {
    /* Reconfigure. sd_alt selects GB-Link's GBC-cable SD path. Must not
     * block: it is called with the shim lock held. */
    void    (*configure)(gbl_phy_mode_t mode, bool sd_alt);
    /* Pull SI up for window_us and report whether it was ever seen high. */
    bool    (*si_seen_high)(uint32_t window_us);
    /* Raw pin snapshot in upstream's bit layout: bit0 = SC LOW, bit1 SI high,
     * bit2 SO high, bit3 SD high, bit4 SD_ALT high. */
    uint8_t (*read_pins)(void);
    /* Float SO and both SD paths, no pulls. */
    void    (*release)(void);
    /* Whether this board has the second SD path fitted at all. */
    bool    has_sd_alt;
} gbl_phy_driver_t;

/* Install the board's driver. Call once before gblink_start(). */
void gbl_phy_install(const gbl_phy_driver_t *drv);

/* ---- called by the driver ------------------------------------------------ */

/* The next word to drive and, as master, the gap to leave after it. Takes
 * the shim lock for the duration of the callback. */
uint16_t gbl_phy_next_tx(uint32_t *gap_loops);

/* A word completed: rx is what the partner drove (LSB-first order already
 * undone), and the last value returned by gbl_phy_next_tx is what we drove. */
void gbl_phy_word_done(uint16_t rx);

/* Words completed since the last configure, for the wrong-pin watchdog. */
uint32_t gbl_phy_words(void);

#ifdef __cplusplus
}
#endif

#endif /* GBAGATE_LINK_PHY_H */
