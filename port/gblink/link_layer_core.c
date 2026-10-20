/*
 * link_layer_core.c -- the vendored layers/linkLayer.h, implemented once over
 * a board's gbl_phy_driver_t.
 *
 * Every decision in here is upstream's (layers/linkLayer_pio.c at 4aef46a);
 * only the bit-moving moved out. In particular:
 *   - Cable detection samples SI with a pull-up over a window, because a GBA
 *     already in Multi-Play drives SO low, which reads as a hard-wired GBA
 *     cable on an instantaneous sample. Any high sample proves a GBC cable.
 *   - Detection runs even when an override is set, so a host that returns
 *     the session to auto gets a fresh answer.
 *   - The wrong-pin flip is granted once per SLAVE configure and only spent
 *     while no word at all has arrived.
 */
#include <stddef.h>

#include <zephyr/kernel.h>

#include "layers/linkLayer.h"
#include "link_phy.h"

static const gbl_phy_driver_t *s_drv;

static ReceiveHandler      s_rx_cb;
static void               *s_rx_ud;
static TransmitHandler     s_tx_cb;
static void               *s_tx_ud;
static TransiveDoneHandler s_done_cb;
static void               *s_done_ud;

static enum LinkMode    s_mode = DISABLED;
static volatile uint8_t s_slave_kind = NONE;
static volatile uint8_t s_flips_left;
static volatile uint32_t s_rx_words;
static uint16_t         s_last_tx;

static bool    s_gbc_detected;
static uint8_t s_override = CABLE_AUTO;

/* The 2 ms window upstream spends sampling (CABLE_DETECT_SAMPLE_ITERS). */
#define CABLE_DETECT_WINDOW_US 2000u

void gbl_phy_install(const gbl_phy_driver_t *drv) { s_drv = drv; }

/* ---- callbacks ------------------------------------------------------------ */

void link_setTransmitCallback(TransmitHandler cb, void *ud) {
    unsigned k = irq_lock(); s_tx_cb = cb; s_tx_ud = ud; irq_unlock(k);
}
void link_setReceiveCallback(ReceiveHandler cb, void *ud) {
    unsigned k = irq_lock(); s_rx_cb = cb; s_rx_ud = ud; irq_unlock(k);
}
void link_setTransiveDoneCallback(TransiveDoneHandler cb, void *ud) {
    unsigned k = irq_lock(); s_done_cb = cb; s_done_ud = ud; irq_unlock(k);
}

uint16_t gbl_phy_next_tx(uint32_t *gap_loops) {
    /* Upstream's placeholder when nothing is registered: 0xDEAD, 50000 loops. */
    struct NextTransmit next = { 0xDEAD, 50000 };
    unsigned k = irq_lock();
    if (s_tx_cb) next = s_tx_cb(s_tx_ud);
    s_last_tx = next.value;
    irq_unlock(k);
    if (gap_loops) *gap_loops = next.timingUs;
    return next.value;
}

void gbl_phy_word_done(uint16_t rx) {
    unsigned k = irq_lock();
    s_rx_words++;
    if (s_rx_cb)   s_rx_cb(rx, s_rx_ud);
    if (s_done_cb) s_done_cb(rx, s_last_tx, s_done_ud);
    irq_unlock(k);
}

uint32_t gbl_phy_words(void)            { return s_rx_words; }
uint32_t link_getReceivedWordCount(void) { return s_rx_words; }

void link_startTransive(void) {}

enum LinkMode link_getMode(void) { return s_mode; }
enum SlaveMode link_getSlaveMode(void) { return (enum SlaveMode)s_slave_kind; }
uint8_t link_getWrongPinFlipsLeft(void) { return s_flips_left; }

/* ---- cable ------------------------------------------------------------------ */

static bool use_gbc(void) {
    bool gbc;
    if (s_override == CABLE_FORCE_GBA)      gbc = false;
    else if (s_override == CABLE_FORCE_GBC) gbc = true;
    else                                    gbc = s_gbc_detected;
    /* A board without the second SD path can only ever use the first; saying
     * otherwise to the host would send it hunting for a cable fault. */
    return gbc && s_drv && s_drv->has_sd_alt;
}

void link_detectCableType(void) {
    s_gbc_detected = s_drv && s_drv->si_seen_high
                   ? s_drv->si_seen_high(CABLE_DETECT_WINDOW_US) : false;
}

enum CableType link_getDetectedCableType(void) { return use_gbc() ? GBC : GBA; }

void link_setCableOverride(uint8_t mode) {
    if (mode > CABLE_FORCE_GBC) mode = CABLE_AUTO;
    s_override = mode;
}

void link_flipSdPinPath(void) {
    s_override = use_gbc() ? CABLE_FORCE_GBA : CABLE_FORCE_GBC;
}

/* ---- modes ---------------------------------------------------------------------- */

static void configure(gbl_phy_mode_t m) {
    s_rx_words = 0;
    if (s_drv && s_drv->configure) s_drv->configure(m, use_gbc());
}

static void configure_slave(void) {
    s_mode = SLAVE;
    configure(GBL_PHY_SLAVE);
    s_slave_kind = CLASSIC;
}

void link_changeMode(enum LinkMode mode) {
    unsigned k = irq_lock();
    switch (mode) {
    case SLAVE:
        s_flips_left = 1; /* consumed by the wrong-pin watchdog */
        configure_slave();
        break;
    case MASTER:
        s_mode = MASTER;
        s_slave_kind = NONE;
        configure(GBL_PHY_MASTER);
        break;
    case DISABLED:
    default:
        s_mode = DISABLED;
        s_slave_kind = NONE;
        configure(GBL_PHY_OFF);
        break;
    }
    irq_unlock(k);
}

bool link_tryWrongPinRecovery(void) {
    unsigned k = irq_lock();
    bool recovered = false;
    if (s_slave_kind == CLASSIC && s_rx_words == 0 && s_flips_left > 0) {
        s_flips_left--;
        link_flipSdPinPath();
        configure_slave();
        recovered = true;
    }
    irq_unlock(k);
    return recovered;
}

void link_configurePartnerPresence(void) {
    unsigned k = irq_lock();
    s_slave_kind = NONE;
    s_mode = SLAVE;
    configure(GBL_PHY_PRESENCE);
    irq_unlock(k);
}

uint8_t link_readPartnerPins(void) {
    return (s_drv && s_drv->read_pins) ? s_drv->read_pins() : 0;
}

void link_releasePartnerPins(void) {
    if (s_drv && s_drv->release) s_drv->release();
}

/* Upstream: the classic slave program, but flagged EREADER so the external
 * watchdog leaves it alone (the e-Reader section runs its own). */
void link_configurePokemonSlave(void) {
    unsigned k = irq_lock();
    s_mode = SLAVE;
    configure(GBL_PHY_SLAVE);
    s_slave_kind = EREADER;
    irq_unlock(k);
}

void link_configureEreaderSlave(void) {
    unsigned k = irq_lock();
    s_mode = SLAVE;
    configure(GBL_PHY_SLAVE_EREADER);
    s_slave_kind = EREADER;
    irq_unlock(k);
}

/* Upstream: the e-Reader slave program (SO held low, the GBA's SD sampled),
 * flagged BATTLE_CHIP_GATE; the chip gate section runs its own wrong-pin
 * check. */
void link_configureBattleChipGateSlave(void) {
    unsigned k = irq_lock();
    s_mode = SLAVE;
    configure(GBL_PHY_SLAVE_EREADER);
    s_slave_kind = BATTLE_CHIP_GATE;
    irq_unlock(k);
}
