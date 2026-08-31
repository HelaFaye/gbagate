/*
 * test_mcp.c -- the I/O expander, against a simulated part.
 *
 *   cc -std=c11 -Wall -Wextra -o test_mcp test_mcp.c mcp23017.c chip_slot.c chip_reader.c
 *
 * The simulator models the register file, auto-increment, and the physics of
 * the pad bus: a line reads low if it is driven low, or if a seated chip ties
 * it to a pad that is. That last part is what makes this a test of the reader
 * rather than of register plumbing.
 */

#include <stdio.h>
#include <string.h>

#include "chip_reader.h"
#include "mcp23017.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-44s got 0x%lX, want 0x%lX\n",
               what, (unsigned long)got, (unsigned long)want);
        g_fail++;
    }
}

/* ---- a simulated MCP23017 with a chip in the slot ------------------------ */

typedef struct {
    uint8_t  reg[0x16];
    uint8_t  addr;
    bool     absent;      /* nothing on the bus: every transfer NAKs */
    bool     impostor;    /* answers, but is not an MCP              */
    bool     bank1;       /* left in the other register layout       */
    uint16_t shorts;      /* pads the seated chip ties to its common */
    uint8_t  common;      /* the chip's common pad, 0 for empty slot */
    bool     det_advance, det_beast;
    unsigned transfers;
} sim_t;

static uint16_t port(const sim_t *s, int reg_a) {
    return (uint16_t)(s->reg[reg_a] | ((uint16_t)s->reg[reg_a + 1] << 8));
}

/* GPIO reads reflect the pins, not a latch: drive, pull-up and chip shorts. */
static uint16_t sim_pins(const sim_t *s) {
    uint16_t iodir = port(s, MCP_IODIRA);
    uint16_t olat  = port(s, MCP_OLATA);

    uint16_t driven_low = (uint16_t)(~iodir & ~olat);
    uint16_t low = driven_low;

    if (s->common && (driven_low & (1u << (s->common - 1)))) {
        low |= s->shorts;
    }
    if (s->det_advance) low |= (1u << 11);
    if (s->det_beast)   low |= (1u << 12);

    return (uint16_t)~low;
}

static bool sim_i2c(void *ctx, uint8_t addr, const uint8_t *w, size_t wlen,
                    uint8_t *r, size_t rlen) {
    sim_t *s = ctx;
    s->transfers++;

    if (s->absent || addr != s->addr) {
        return false;
    }
    if (wlen == 0) {
        return false;
    }

    uint8_t reg = w[0];
    if (s->bank1) {
        /* In BANK=1 the numbering differs, so every access lands somewhere
         * else. Modelled crudely: it answers, with the wrong data. */
        reg = (uint8_t)((reg + 0x10) & 0x1F);
        if (reg >= sizeof s->reg) {
            reg = 0;
        }
        if (w[0] == MCP_IOCON && wlen >= 2) {
            s->bank1 = false;      /* the BANK=0 write lands and takes effect */
            return true;
        }
    }

    for (size_t i = 1; i < wlen; i++) {
        if (reg + (i - 1) < sizeof s->reg) {
            s->reg[reg + i - 1] = w[i];
        }
    }

    for (size_t i = 0; i < rlen; i++) {
        uint8_t rr = (uint8_t)(reg + i);
        if (rr == MCP_GPIOA || rr == MCP_GPIOB) {
            uint16_t pins = sim_pins(s);
            r[i] = (rr == MCP_GPIOA) ? (uint8_t)(pins & 0xFF)
                                     : (uint8_t)(pins >> 8);
        } else if (rr == MCP_IOCON && s->impostor) {
            r[i] = 0x01;           /* the unimplemented bit reads set */
        } else if (rr < sizeof s->reg) {
            r[i] = s->reg[rr];
        } else {
            r[i] = 0xFF;
        }
    }
    return true;
}

static sim_t make_sim(void) {
    sim_t s;
    memset(&s, 0, sizeof s);
    s.addr = MCP_ADDR_BASE;
    memset(s.reg + MCP_IODIRA, 0xFF, 2);   /* power-on: all inputs */
    return s;
}

static uint16_t bits(const uint8_t *pads, size_t n) {
    uint16_t m = 0;
    for (size_t i = 0; i < n; i++) {
        m |= (uint16_t)(1u << (pads[i] - 1));
    }
    return m;
}

/* ---- tests --------------------------------------------------------------- */

static void test_presence(void) {
    printf("presence\n");

    sim_t sim = make_sim();
    mcp_t m;
    check("a real part is found", mcp_init(&m, sim_i2c, &sim, MCP_ADDR_BASE), 1);
    check("marked ready", m.ready, 1);

    /* No board fitted is the normal case, not an error: Read Chip is hidden
     * rather than offered and broken. */
    sim_t none = make_sim();
    none.absent = true;
    mcp_t m2;
    check("absent board reported", mcp_init(&m2, sim_i2c, &none, MCP_ADDR_BASE), 0);
    check("not ready", m2.ready, 0);

    /* Something else answering at 0x20 must not be mistaken for an expander. */
    sim_t fake = make_sim();
    fake.impostor = true;
    mcp_t m3;
    check("impostor rejected", mcp_init(&m3, sim_i2c, &fake, MCP_ADDR_BASE), 0);

    /* Wrong address: nothing answers. */
    sim_t sim2 = make_sim();
    mcp_t m4;
    check("wrong address", mcp_init(&m4, sim_i2c, &sim2, MCP_ADDR_BASE + 3), 0);
}

static void test_bank1_recovery(void) {
    printf("a part left in BANK=1\n");

    /*
     * IOCON.BANK renumbers every register. Firmware that crashed with BANK=1
     * set leaves the part that way across a reset, and every value read
     * afterwards is plausible nonsense rather than an obvious failure.
     * mcp_init must clear it before trusting anything.
     */
    sim_t sim = make_sim();
    sim.bank1 = true;

    mcp_t m;
    check("recovers and initialises", mcp_init(&m, sim_i2c, &sim, MCP_ADDR_BASE), 1);
    check("bank cleared", sim.bank1, 0);
}

static void test_bus_discipline(void) {
    printf("bus discipline\n");

    sim_t sim = make_sim();
    mcp_t m;
    mcp_init(&m, sim_i2c, &sim, MCP_ADDR_BASE);

    const chip_bus_io_t *io = mcp_bus_io();

    /* At rest every pad line is an input with a pull-up. */
    check("idle: all pads input", port(&sim, MCP_IODIRA) & SLOT_PAD_MASK,
          SLOT_PAD_MASK);
    check("idle: pull-ups on", port(&sim, MCP_GPPUA) & SLOT_PAD_MASK,
          SLOT_PAD_MASK);

    /* Drive pad 10 low, as an Advance read does. */
    uint16_t common = (uint16_t)(1u << 9);
    io->set_out(&m, (uint16_t)(SLOT_PAD_MASK & ~common));
    io->set_dir(&m, (uint16_t)(SLOT_PAD_MASK & ~common));
    check("exactly one line driven", (uint16_t)(~port(&sim, MCP_IODIRA) & SLOT_PAD_MASK),
          common);

    /* The detect switches must stay inputs: driving one fights the switch. */
    check("detect lines stay inputs",
          port(&sim, MCP_IODIRA) & 0x1800, 0x1800);

    io->set_dir(&m, SLOT_PAD_MASK);
    check("released", (uint16_t)(~port(&sim, MCP_IODIRA) & SLOT_PAD_MASK), 0);

    /*
     * A redundant call must not generate traffic: the pad bus changes
     * direction on every scan and this runs at the UI's rate.
     *
     * Settle both registers first. Releasing the direction alone leaves the
     * output latch still holding the common pad low, so the next set_out is a
     * real change and legitimately costs a transaction.
     */
    io->set_out(&m, SLOT_PAD_MASK);
    unsigned before = sim.transfers;
    io->set_dir(&m, SLOT_PAD_MASK);
    io->set_out(&m, SLOT_PAD_MASK);
    check("no traffic when nothing changed", sim.transfers, before);
}

static void test_reads_a_chip(void) {
    printf("reading a seated chip\n");

    /* Gun_Del_Sol from the EXE project: pads 4, 10 and 11 in one net, with
     * pad 10 the Advance common. */
    static const uint8_t gds[] = { 4, 11 };
    sim_t sim = make_sim();
    sim.common = 10;
    sim.shorts = bits(gds, 2);
    sim.det_advance = true;

    mcp_t m;
    mcp_init(&m, sim_i2c, &sim, MCP_ADDR_BASE);

    bool adv = false, beast = false;
    check("detect read", mcp_read_detect(&m, &adv, &beast), 1);
    check("advance slot occupied", adv, 1);
    check("beast slot empty", beast, 0);

    chip_slots_t slots;
    chip_slots_init(&slots, mcp_bus_io(), &m);

    chip_family_t fam;
    uint16_t pads;
    check("scan succeeded", chip_slots_scan(&slots, adv, beast, &fam, &pads),
          SLOT_SCAN_OK);
    check("family", fam, CHIP_FAMILY_ADVANCE);
    check("pad 4 shorted", (pads & (1u << 3)) == 0, 1);
    check("pad 11 shorted", (pads & (1u << 10)) == 0, 1);
    check("pad 1 open", (pads & (1u << 0)) != 0, 1);
    check("common forced high", (pads & (1u << 9)) != 0, 1);

    chip_reader_t r;
    chip_reader_init(&r, CHIP_FAMILY_ADVANCE, NULL, NULL);
    printf("  decodes to 0x%03X\n", chip_reader_decode(&r, pads));

    /* And the bus is left at rest afterwards. */
    check("bus released after the scan",
          (uint16_t)(~port(&sim, MCP_IODIRA) & SLOT_PAD_MASK), 0);
}

static void test_io_failure(void) {
    printf("bus failure mid-scan\n");

    sim_t sim = make_sim();
    sim.common = 2;
    sim.det_beast = true;
    mcp_t m;
    mcp_init(&m, sim_i2c, &sim, MCP_ADDR_BASE);

    chip_slots_t slots;
    chip_slots_init(&slots, mcp_bus_io(), &m);

    /* The board is unplugged between transactions. The scan must fail, and it
     * must not leave a line driven -- a common pad still driven would short
     * against the next chip inserted. */
    sim.absent = true;
    check("failure reported",
          chip_slots_scan(&slots, false, true, NULL, NULL), SLOT_SCAN_IO_ERROR);
    check("errors counted", m.errors > 0, 1);
}

int main(void) {
    test_presence();
    test_bank1_recovery();
    test_bus_discipline();
    test_reads_a_chip();
    test_io_failure();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
