/*
 * test_link_core.c -- port/gblink/link_layer_core.c on its own, under ASan
 * and UBSan. test_gblink.py runs it.
 *
 * A PHY can finish a word it started before link_changeMode(DISABLED). The
 * packet layer whose callbacks would take it is a member of a UsbSection on
 * the link module's stack, gone by then: the simulator caught exactly that as
 * a stack-use-after-scope one run in twenty (wireless_cable_peer_gone). Once
 * the link is disabled, no callback may run.
 */
#include <stdio.h>

#include "layers/linkLayer.h"
#include "link_phy.h"

static int s_rx, s_tx, s_done, s_fail;

static void on_rx(uint16_t v, void *ud) { (void)v; (void)ud; s_rx++; }
static struct NextTransmit on_tx(void *ud) { (void)ud; s_tx++; return (struct NextTransmit){ 0x1234, 100 }; }
static void on_done(uint16_t rx, uint16_t tx, void *ud) { (void)rx; (void)tx; (void)ud; s_done++; }

static void configure(gbl_phy_mode_t m, bool sd_alt) { (void)m; (void)sd_alt; }
static bool si_seen_high(uint32_t us) { (void)us; return false; }
static uint8_t read_pins(void) { return 0; }
static void release(void) {}
static const gbl_phy_driver_t k_drv = { configure, si_seen_high, read_pins, release, false };

static void check(const char *what, int got, int want) {
    if (got != want) { printf("  FAIL %s: got %d, want %d\n", what, got, want); s_fail++; }
}

/* Storage the shim's flash map wants; nothing here uses it. */
int zshim_storage_read(size_t off, void *dst, size_t len) { (void)off; (void)dst; (void)len; return -1; }
int zshim_storage_write(size_t off, const void *src, size_t len) { (void)off; (void)src; (void)len; return -1; }
int zshim_storage_erase(size_t off, size_t len) { (void)off; (void)len; return -1; }

int main(void) {
    gbl_phy_install(&k_drv);
    link_setTransmitCallback(on_tx, NULL);
    link_setReceiveCallback(on_rx, NULL);
    link_setTransiveDoneCallback(on_done, NULL);

    link_changeMode(MASTER);
    check("an active link asks for the next word", gbl_phy_next_tx(NULL), 0x1234);
    gbl_phy_word_done(0x5678);
    check("and delivers the word", s_rx == 1 && s_done == 1, 1);

    link_changeMode(DISABLED);
    const int tx = s_tx;
    check("a disabled link does not ask for one", gbl_phy_next_tx(NULL), 0xDEAD);
    gbl_phy_word_done(0x9ABC);   /* a word the PHY had already started */
    check("nor calls the packet layer", s_tx == tx && s_rx == 1 && s_done == 1, 1);

    link_changeMode(SLAVE);
    gbl_phy_word_done(0x1111);
    check("enabled again, it does", s_rx == 2 && s_done == 2, 1);

    puts(s_fail ? "link core: FAILED" : "link core: no callback after DISABLED");
    return s_fail != 0;
}
