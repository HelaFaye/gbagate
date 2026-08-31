/*
 * mcp23017.h -- the I/O expander behind the chip reader's pad bus.
 *
 * chip_slot.c already defines what it needs: set the direction of sixteen
 * lines, set the output levels, read them back. This is that, over I2C, for an
 * MCP23017 -- and it is the last piece between a fitted reader board and a
 * working Read Chip mode.
 *
 * The I2C transfer is injected, so register sequencing, the presence probe and
 * every failure path are testable on a host against a simulated part. The same
 * arrangement as nrf24_probe.c, and for the same reason: this is where a
 * transposed register number costs an afternoon with a logic analyser.
 *
 * Only port A and the low three bits of port B are used -- eleven pad lines
 * plus two detect switches, thirteen of sixteen. See the note in chip_slot.h
 * about why eleven and not twenty-one.
 */

#ifndef MCP23017_H
#define MCP23017_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chip_slot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Base address; A2..A0 are strapped on the board. */
#define MCP_ADDR_BASE 0x20

/*
 * Register numbers in the IOCON.BANK=0 layout, which is the power-on default
 * and the one every example uses. BANK=1 renumbers everything, so a part left
 * in that state by earlier firmware reads as garbage -- mcp_init() forces
 * BANK=0 before trusting anything else.
 */
#define MCP_IODIRA   0x00
#define MCP_IODIRB   0x01
#define MCP_IPOLA    0x02
#define MCP_GPINTENA 0x04
#define MCP_IOCON    0x0A
#define MCP_GPPUA    0x0C
#define MCP_GPPUB    0x0D
#define MCP_GPIOA    0x12
#define MCP_GPIOB    0x13
#define MCP_OLATA    0x14
#define MCP_OLATB    0x15

/*
 * One I2C transaction: write `wlen` bytes, then read `rlen` bytes. Either may
 * be zero. Returns false on NAK or bus error.
 */
typedef bool (*mcp_i2c_fn)(void *ctx, uint8_t addr,
                           const uint8_t *w, size_t wlen,
                           uint8_t *r, size_t rlen);

typedef struct {
    mcp_i2c_fn i2c;
    void      *ctx;
    uint8_t    addr;

    /* Shadow of what we last wrote, so a read-modify-write never has to read.
     * The pad bus flips direction on every scan; a round trip per line would
     * make a scan cost sixteen transactions instead of two. */
    uint16_t iodir;   /* 1 = input */
    uint16_t olat;    /* output latch */
    uint16_t gppu;    /* 1 = pull-up enabled */

    uint32_t errors;
    bool     ready;
} mcp_t;

/*
 * Probe and configure. Returns false if nothing answers at this address, which
 * is the normal case on a board with no reader fitted -- Read Chip is then
 * hidden rather than offered and broken.
 */
bool mcp_init(mcp_t *m, mcp_i2c_fn i2c, void *ctx, uint8_t addr);

/* Is a part actually answering? Cheap enough to call on a rescan. */
bool mcp_present(mcp_t *m);

/* The chip_slot.c interface, backed by this expander. */
const chip_bus_io_t *mcp_bus_io(void);

/* Read the two detect switches: bit 0 = Advance slot, bit 1 = Beast slot. */
bool mcp_read_detect(mcp_t *m, bool *advance, bool *beast);

#ifdef __cplusplus
}
#endif

#endif /* MCP23017_H */
