/*
 * nrf24_probe.c -- see nrf24_probe.h
 */

#include <string.h>

#include "nrf24_probe.h"

static uint8_t rd_reg(nrf24_xfer_fn xfer, void *ctx, uint8_t reg) {
    uint8_t tx[2] = { (uint8_t)(NRF24_CMD_R_REGISTER | (reg & NRF24_REG_MASK)), 0xFF };
    uint8_t rx[2] = { 0, 0 };
    xfer(ctx, tx, rx, 2);
    return rx[1];
}

static void wr_reg(nrf24_xfer_fn xfer, void *ctx, uint8_t reg, uint8_t val) {
    uint8_t tx[2] = { (uint8_t)(NRF24_CMD_W_REGISTER | (reg & NRF24_REG_MASK)), val };
    uint8_t rx[2] = { 0, 0 };
    xfer(ctx, tx, rx, 2);
}

static void rd_buf(nrf24_xfer_fn xfer, void *ctx, uint8_t reg, uint8_t *buf, size_t n) {
    uint8_t tx[8], rx[8];
    if (n > 5) {
        n = 5;
    }
    memset(tx, 0xFF, sizeof tx);
    tx[0] = (uint8_t)(NRF24_CMD_R_REGISTER | (reg & NRF24_REG_MASK));
    xfer(ctx, tx, rx, n + 1);
    memcpy(buf, rx + 1, n);
}

static void wr_buf(nrf24_xfer_fn xfer, void *ctx, uint8_t reg, const uint8_t *buf, size_t n) {
    uint8_t tx[8], rx[8];
    if (n > 5) {
        n = 5;
    }
    tx[0] = (uint8_t)(NRF24_CMD_W_REGISTER | (reg & NRF24_REG_MASK));
    memcpy(tx + 1, buf, n);
    xfer(ctx, tx, rx, n + 1);
}

void nrf24_probe(nrf24_xfer_fn xfer, void *ctx, nrf24_probe_t *out) {
    memset(out, 0, sizeof *out);

    if (!xfer) {
        out->reason = "no SPI transfer function";
        return;
    }

    /* 1. STATUS. A floating MISO gives 0x00 or 0xFF; neither is a legal
     *    STATUS value, since bit 7 is reserved-zero and the FIFO bits cannot
     *    all be set at once. */
    out->status = rd_reg(xfer, ctx, NRF24_REG_STATUS);
    if (out->status == 0x00 || out->status == 0xFF) {
        out->reason = "bus floating (STATUS 0x00/0xFF)";
        return;
    }
    if (out->status & 0x80) {
        out->reason = "STATUS reserved bit set";
        return;
    }

    /* 2. SETUP_AW: bits 7:2 are hardwired zero, bits 1:0 are 1..3. */
    out->setup_aw = rd_reg(xfer, ctx, NRF24_REG_SETUP_AW);
    if ((out->setup_aw & 0xFC) != 0 || (out->setup_aw & 0x03) == 0) {
        out->reason = "SETUP_AW implausible";
        return;
    }

    /* 3. The real test: five-byte write and read back on TX_ADDR. */
    uint8_t saved[5];
    rd_buf(xfer, ctx, NRF24_REG_TX_ADDR, saved, 5);

    static const uint8_t pattern[5] = { 0xC2, 0x5A, 0xA5, 0x3C, 0x91 };
    uint8_t seen[5];
    wr_buf(xfer, ctx, NRF24_REG_TX_ADDR, pattern, 5);
    rd_buf(xfer, ctx, NRF24_REG_TX_ADDR, seen, 5);
    wr_buf(xfer, ctx, NRF24_REG_TX_ADDR, saved, 5);

    if (memcmp(pattern, seen, 5) != 0) {
        out->reason = "TX_ADDR did not read back";
        return;
    }

    out->present = true;

    /*
     * 4. Variant. RF_DR_LOW selects 250 kbps and exists only on the "+" part.
     *    Clones commonly accept the write and drop the bit. Not fatal -- a
     *    clone still works at 1 and 2 Mbps -- but worth surfacing, because
     *    250 kbps is exactly the mode you would reach for to buy range.
     */
    uint8_t rf_saved = rd_reg(xfer, ctx, NRF24_REG_RF_SETUP);
    wr_reg(xfer, ctx, NRF24_REG_RF_SETUP, (uint8_t)(rf_saved | NRF24_RF_DR_LOW));
    uint8_t probed = rd_reg(xfer, ctx, NRF24_REG_RF_SETUP);
    wr_reg(xfer, ctx, NRF24_REG_RF_SETUP, rf_saved);

    out->rf_setup = rd_reg(xfer, ctx, NRF24_REG_RF_SETUP);
    out->plus     = (probed & NRF24_RF_DR_LOW) != 0;
}
