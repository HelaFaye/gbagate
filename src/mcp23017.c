/*
 * mcp23017.c -- see mcp23017.h
 */

#include <string.h>

#include "mcp23017.h"

/*
 * The detect switches sit above the pad lines. Pads 1..11 occupy bits 0..10,
 * so GPB0 and GPB1 -- bits 8 and 9 -- are pads 9 and 10, not spare pins. Using
 * them collided: a direction change for a pad silently became a change for a
 * switch, the mask came out all-ones, and the write was skipped as redundant,
 * so nothing was ever driven and every chip decoded as empty.
 *
 * Bits 11 and 12 are GPB3 and GPB4, the first pins genuinely past the bus.
 */
#define DETECT_ADVANCE_BIT 11  /* GPB3 */
#define DETECT_BEAST_BIT   12  /* GPB4 */

static mcp_t *s_bound;   /* the instance chip_slot.c talks to */

/* ---- register access ---------------------------------------------------- */

static bool wr8(mcp_t *m, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    if (!m->i2c || !m->i2c(m->ctx, m->addr, buf, 2, NULL, 0)) {
        m->errors++;
        return false;
    }
    return true;
}

static bool rd8(mcp_t *m, uint8_t reg, uint8_t *val) {
    if (!m->i2c || !m->i2c(m->ctx, m->addr, &reg, 1, val, 1)) {
        m->errors++;
        return false;
    }
    return true;
}

/*
 * Both ports in one transaction. The MCP auto-increments within a bank, so
 * addressing IODIRA and reading or writing two bytes covers IODIRB too --
 * which halves the traffic on the pad bus, where direction changes on every
 * single scan.
 */
static bool wr16(mcp_t *m, uint8_t reg_a, uint16_t val) {
    uint8_t buf[3] = { reg_a, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
    if (!m->i2c || !m->i2c(m->ctx, m->addr, buf, 3, NULL, 0)) {
        m->errors++;
        return false;
    }
    return true;
}

static bool rd16(mcp_t *m, uint8_t reg_a, uint16_t *val) {
    uint8_t out[2];
    if (!m->i2c || !m->i2c(m->ctx, m->addr, &reg_a, 1, out, 2)) {
        m->errors++;
        return false;
    }
    *val = (uint16_t)(out[0] | ((uint16_t)out[1] << 8));
    return true;
}

/* ---- presence ----------------------------------------------------------- */

/*
 * A floating I2C bus NAKs, so a successful transfer already means something is
 * there. It does not mean it is an MCP23017: confirm with a register that has
 * hardwired behaviour. IOCON's bit 0 is unimplemented and always reads zero,
 * and IODIR defaults to all-inputs, so a write-and-read-back of a pattern is
 * the decisive test -- the same reasoning as the nRF24 probe.
 */
bool mcp_present(mcp_t *m) {
    uint8_t v;
    if (!rd8(m, MCP_IOCON, &v)) {
        return false;
    }
    if (v & 0x01) {
        return false;   /* unimplemented bit reads as set: not an MCP */
    }

    uint8_t saved;
    if (!rd8(m, MCP_GPPUA, &saved)) {
        return false;
    }
    if (!wr8(m, MCP_GPPUA, 0xA5)) {
        return false;
    }
    uint8_t back = 0;
    bool ok = rd8(m, MCP_GPPUA, &back) && back == 0xA5;
    wr8(m, MCP_GPPUA, saved);   /* restore whatever was there */
    return ok;
}

bool mcp_init(mcp_t *m, mcp_i2c_fn i2c, void *ctx, uint8_t addr) {
    memset(m, 0, sizeof *m);
    m->i2c  = i2c;
    m->ctx  = ctx;
    m->addr = addr;

    /*
     * Force the register layout before reading anything else. A part left in
     * IOCON.BANK=1 by earlier firmware answers on completely different
     * register numbers, and every value read would be plausible nonsense.
     * Writing 0x05 to 0x0A is a no-op in BANK=0 and clears BANK in BANK=1.
     */
    if (!wr8(m, MCP_IOCON, 0x00)) {
        return false;
    }

    if (!mcp_present(m)) {
        return false;
    }

    /*
     * Everything an input with a pull-up, matching the pad bus at rest: a chip
     * shorts a pad to the common line, and the common line is only driven low
     * for the moment of a read. See the bus discipline note in chip_slot.h.
     */
    m->iodir = 0xFFFF;
    m->olat  = 0xFFFF;
    m->gppu  = 0xFFFF;

    if (!wr16(m, MCP_IODIRA, m->iodir) ||
        !wr16(m, MCP_GPPUA, m->gppu) ||
        !wr16(m, MCP_OLATA, m->olat)) {
        return false;
    }

    m->ready = true;
    s_bound  = m;
    return true;
}

/* ---- the chip_slot.c interface ------------------------------------------ */

static bool bus_set_dir(void *ctx, uint16_t input_mask) {
    mcp_t *m = ctx ? ctx : s_bound;
    if (!m || !m->ready) {
        return false;
    }
    /* Only the eleven pad lines are ours to move; the detect switches stay
     * inputs, and driving one would fight the switch. */
    uint16_t want = (uint16_t)((input_mask & SLOT_PAD_MASK) |
                               (m->iodir & ~SLOT_PAD_MASK) |
                               (1u << DETECT_ADVANCE_BIT) |
                               (1u << DETECT_BEAST_BIT));
    if (want == m->iodir) {
        return true;   /* nothing to say */
    }
    if (!wr16(m, MCP_IODIRA, want)) {
        return false;
    }
    m->iodir = want;
    return true;
}

static bool bus_set_out(void *ctx, uint16_t levels) {
    mcp_t *m = ctx ? ctx : s_bound;
    if (!m || !m->ready) {
        return false;
    }
    uint16_t want = (uint16_t)((levels & SLOT_PAD_MASK) |
                               (m->olat & ~SLOT_PAD_MASK));
    if (want == m->olat) {
        return true;
    }
    if (!wr16(m, MCP_OLATA, want)) {
        return false;
    }
    m->olat = want;
    return true;
}

static bool bus_get_in(void *ctx, uint16_t *levels) {
    mcp_t *m = ctx ? ctx : s_bound;
    if (!m || !m->ready || !levels) {
        return false;
    }
    return rd16(m, MCP_GPIOA, levels);
}

static const chip_bus_io_t k_bus = { bus_set_dir, bus_set_out, bus_get_in };

const chip_bus_io_t *mcp_bus_io(void) { return &k_bus; }

/* ---- detect switches ---------------------------------------------------- */

bool mcp_read_detect(mcp_t *m, bool *advance, bool *beast) {
    if (!m || !m->ready) {
        return false;
    }
    uint16_t v;
    if (!rd16(m, MCP_GPIOA, &v)) {
        return false;
    }
    /* Switches pull to ground when a chip is seated, and the pull-ups hold
     * them high otherwise -- so closed reads low. */
    if (advance) {
        *advance = (v & (1u << DETECT_ADVANCE_BIT)) == 0;
    }
    if (beast) {
        *beast = (v & (1u << DETECT_BEAST_BIT)) == 0;
    }
    return true;
}

/* ---- as a button bank --------------------------------------------------- */

bool mcp_buttons_init(mcp_t *m, mcp_i2c_fn i2c, void *ctx, uint8_t addr) {
    memset(m, 0, sizeof *m);
    m->i2c  = i2c;
    m->ctx  = ctx;
    m->addr = addr;

    /* Same reason as mcp_init(): a part left in BANK=1 answers on different
     * register numbers and every value read is plausible nonsense. */
    if (!wr8(m, MCP_IOCON, 0x00)) {
        return false;
    }
    if (!mcp_present(m)) {
        return false;
    }

    /*
     * All sixteen pins inputs with pull-ups. Unlike the pad bus, nothing here
     * is ever driven -- a button bank has no output phase, so the direction is
     * set once and never changes. That also means an unpopulated pin reads
     * high, which is "not pressed", so a board that fits fewer than sixteen
     * switches needs no special handling beyond the mode mask ui_input.c
     * already applies.
     */
    m->iodir = 0xFFFF;
    m->gppu  = 0xFFFF;
    m->olat  = 0xFFFF;

    if (!wr16(m, MCP_IODIRA, m->iodir) || !wr16(m, MCP_GPPUA, m->gppu)) {
        return false;
    }

    m->ready = true;
    return true;
}

uint32_t mcp_buttons_read(void *ctx) {
    mcp_t *m = ctx;
    if (!m || !m->ready) {
        return 0;
    }
    uint16_t v;
    if (!rd16(m, MCP_GPIOA, &v)) {
        /*
         * A failed read must report "nothing pressed", not the last value or
         * all-ones. Returning stale state would leave a button stuck down
         * across a bus glitch, and all-ones would look like every button held.
         */
        return 0;
    }
    return (uint32_t)(uint16_t)~v;   /* active low */
}
