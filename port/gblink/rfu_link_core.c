/*
 * rfu_link_core.c -- the vendored layers/linkLayer_rfu.h over a board's
 * gbl_rfu_phy_t. See rfu_phy.h for the wire contract; this file is only
 * bookkeeping, so it is the same on the ESP32-S3 and in the host simulator.
 *
 * Derived from GB-Link's adapter firmware, which is GPL-3.0
 * (third_party/gblink-firmware/LICENSE); this file is under that licence.
 */
#include "rfu_phy.h"

#include <zephyr/kernel.h>

#include "zshim.h"

static const gbl_rfu_phy_t *s_drv;
static RfuTransferDone      s_done_cb;
static void                *s_done_ud;
static enum RfuRole         s_role = RFU_ROLE_GBA_MASTER;
static volatile bool        s_enabled;
static uint8_t              s_last_reset_pin = RFU_PIN_NONE;

/* Upstream's transfer-path timing, reported by command 0x4C. */
static volatile uint32_t s_isr_max_us, s_isr_late, s_isr_gap_min_us = 0xFFFFFFFFu, s_isr_count;
static int64_t           s_isr_last_start;

void gbl_rfu_install(const gbl_rfu_phy_t *drv) { s_drv = drv; }

void rfuLink_setDoneCallback(RfuTransferDone cb, void *userData) {
    unsigned k = irq_lock();
    s_done_ud = userData;
    s_done_cb = cb;
    irq_unlock(k);
}

void gbl_rfu_word_done(uint32_t rx) {
    const int64_t start = zshim_now_us();
    unsigned k = irq_lock();
    if (s_enabled && s_role == RFU_ROLE_GBA_MASTER && s_done_cb) s_done_cb(rx, s_done_ud);
    irq_unlock(k);

    const uint32_t took = (uint32_t)(zshim_now_us() - start);
    if (took > s_isr_max_us) s_isr_max_us = took;
    if (took > RFU_ISR_BUDGET_US) s_isr_late++;
    if (s_isr_count != 0) {
        const uint32_t gap = (uint32_t)(start - s_isr_last_start);
        if (gap < s_isr_gap_min_us) s_isr_gap_min_us = gap;
    }
    s_isr_last_start = start;
    s_isr_count++;
}

void rfuLink_enable(void) {
    if (!s_drv || s_enabled) return;
    s_drv->enable();
    s_role = RFU_ROLE_GBA_MASTER;
    unsigned k = irq_lock();
    s_enabled = true;
    irq_unlock(k);
    s_drv->stage(0);   /* the RESET-state response */
}

void rfuLink_disable(void) {
    if (!s_drv || !s_enabled) return;
    unsigned k = irq_lock();
    s_enabled = false;
    irq_unlock(k);
    s_drv->disable();
}

void rfuLink_setRole(enum RfuRole role) {
    if (!s_enabled || role == s_role) return;
    s_drv->set_role(role);
    s_role = role;
}

enum RfuRole rfuLink_getRole(void) { return s_role; }

bool rfuLink_masterExchange(uint32_t word, uint32_t *rx, uint32_t timeoutUs) {
    if (!s_enabled || s_role != RFU_ROLE_ADAPTER_MASTER) return false;
    return s_drv->exchange(word, rx, timeoutUs);
}

void rfuLink_masterDriveSi(bool high) {
    if (!s_enabled || s_role != RFU_ROLE_ADAPTER_MASTER) return;
    s_drv->drive_si(high);
}

/* Upstream queues into the active state machine; only the GBA_MASTER one ever
 * receives a push, since the section pushes right after swapping back. */
void rfuLink_pushTx(uint32_t word) {
    if (!s_enabled || s_role != RFU_ROLE_GBA_MASTER) return;
    s_drv->stage(word);
}

bool rfuLink_gbaLineHigh(void) { return s_drv && s_drv->so_high(); }

void rfuLink_isrStats(uint32_t out[5]) {
    out[0] = s_isr_max_us;
    out[1] = s_isr_late;
    out[2] = s_isr_gap_min_us == 0xFFFFFFFFu ? 0 : s_isr_gap_min_us;
    out[3] = s_isr_count;
    out[4] = s_last_reset_pin;
}

void rfuLink_debugSnapshot(uint8_t *smPc, uint8_t *txLvl, uint8_t *rxLvl, uint8_t *lines) {
    /* No state machine to report a program counter or FIFO levels for. */
    *smPc = 0;
    *txLvl = 0;
    *rxLvl = 0;
    *lines = (uint8_t)((s_drv ? s_drv->lines() : 0) | (s_role == RFU_ROLE_ADAPTER_MASTER ? 8 : 0));
}

void rfuLink_waitResetEnd(void) {
    if (!s_enabled || s_last_reset_pin == RFU_PIN_NONE) return;
    s_drv->wait_reset_end();
}

void rfuLink_realign(void) {
    if (!s_enabled || s_role != RFU_ROLE_GBA_MASTER) return;
    s_drv->realign();
}

bool rfuLink_sdResetSeen(void) {
    if (!s_enabled) return false;
    uint8_t pin = RFU_PIN_NONE;
    if (!s_drv->sd_reset_seen(&pin)) return false;
    s_last_reset_pin = pin;
    return true;
}
