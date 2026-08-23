/*
 * nrf24_probe.h -- detect whether an nRF24 radio is fitted, at boot, over SPI.
 *
 * The SPI transfer is injected so this can be unit tested against a simulated
 * radio, a simulated clone and an empty bus. On the S3 you pass a wrapper
 * around spi_device_polling_transmit.
 *
 * What "present" means here
 * ------------------------
 * A floating SPI bus reads as all-zero or all-one depending on the pull, and a
 * shorted CSN reads as garbage. So presence is not "did we get bytes back", it
 * is "did we get bytes back that only a real device could produce":
 *
 *   1. STATUS must not be 0x00 or 0xFF.
 *   2. SETUP_AW has six hardwired zero bits and a low field that is 1..3.
 *   3. A write to TX_ADDR must read back exactly, then restore.
 *
 * Step 3 is the one that matters. Steps 1 and 2 can pass on a floating bus with
 * the right luck; a five-byte write-readback cannot.
 */

#ifndef NRF24_PROBE_H
#define NRF24_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registers and commands used by the probe. */
#define NRF24_CMD_R_REGISTER 0x00
#define NRF24_CMD_W_REGISTER 0x20
#define NRF24_CMD_NOP        0xFF
#define NRF24_REG_MASK       0x1F

#define NRF24_REG_CONFIG   0x00
#define NRF24_REG_SETUP_AW 0x03
#define NRF24_REG_RF_CH    0x05
#define NRF24_REG_RF_SETUP 0x06
#define NRF24_REG_STATUS   0x07
#define NRF24_REG_TX_ADDR  0x10

/* RF_DR_LOW exists only on the "+" silicon. Writing it and reading it back is
 * the standard way to tell a genuine nRF24L01+ from the original part and from
 * the many clones that ignore the bit. */
#define NRF24_RF_DR_LOW  0x20
#define NRF24_RF_DR_HIGH 0x08

/*
 * One CS-asserted SPI transaction. tx and rx are both len bytes; rx may alias
 * tx. Implementations must assert CSN, clock len bytes, and deassert.
 */
typedef void (*nrf24_xfer_fn)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);

typedef struct {
    bool    present;      /* something on the bus answered like an nRF24 */
    bool    plus;         /* ...and RF_DR_LOW stuck, so it is an L01+    */
    uint8_t status;       /* raw STATUS as read                          */
    uint8_t setup_aw;     /* raw SETUP_AW                                */
    uint8_t rf_setup;     /* raw RF_SETUP after restore                  */
    const char *reason;   /* why the probe failed, NULL on success       */
} nrf24_probe_t;

/* Probe the radio. Leaves every register as it found it. */
void nrf24_probe(nrf24_xfer_fn xfer, void *ctx, nrf24_probe_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NRF24_PROBE_H */
