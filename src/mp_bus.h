/*
 * mp_bus.h -- the board's side of one Multi-Player transfer, in every slot
 * it answers, on the carrier's chain of up to three GBAs (doc/CARRIER.md).
 *
 * The wire (port/esp32s3/gba_link_esp32s3.c has the derivation from
 * GB-Link's PIO programs):
 *   - SC: the parent holds it low for the whole transfer.
 *   - SD: one shared line; each unit in turn sends start bit, 16 data bits
 *     LSB first, stop bit, at 115200 baud (8.68 us a bit), push-pull, then
 *     lets go. Every unit hears every word, in slot order.
 *   - SI/SO: the turn. A unit's SO goes low when it has sent; that is the
 *     next unit's SI. The parent is the unit whose SI is low at rest.
 *
 * The chain on the carrier: board SO -> port 1 SI, port 1 SO -> port 2 SI,
 * port 2 SO -> port 3 SI, and the board reads every port's SO (SI1..SI3).
 * Local GBAs fill ports 1..locals in order, and the board answers every
 * other slot. So the board's slots are either the ones after the last
 * local GBA (a local GBA is the parent), or slot 0 and those after
 * (a remote player is the parent and the board stands in for it):
 *
 *   local parent   slot 0 port 1, 1 port 2, ...; board: locals..players-1
 *   remote parent  board: 0; slot 1 port 1, ...; board: locals+1..players-1
 *
 * When the board answers two slots in a row it sends them back to back, as
 * two GBAs would.
 *
 * Pins and time come through mp_bus_hal_t, so the same code runs in the
 * ESP32-S3's interrupt and against test/test_mpbus.c's model of real GBAs.
 * Time is in the HAL's ticks (the S3's CPU cycles; the test's nanoseconds),
 * ticks_per_us of them a microsecond. It wraps; only differences are used,
 * which is why it is not converted to nanoseconds: a cycle counter's wrap
 * would be a jump in the middle of a transfer.
 */
#ifndef MP_BUS_H
#define MP_BUS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Timing in ns, as gba_link_esp32s3.c derives it from the PIO programs. */
#define MPB_NS_BIT           8680u   /* 115200 baud                          */
#define MPB_NS_SAMPLE0      13020u   /* SC falling -> the parent's first data bit's middle */
#define MPB_NS_START_TO_MID 12480u   /* a start edge seen by polling -> first data bit's middle */
#define MPB_NS_TURN          4340u   /* the turn passed (SI low) -> our start bit */
#define MPB_NS_BACK_TO_BACK  4340u   /* our stop bit -> our next slot's start bit */
#define MPB_NS_SLOT_TIMEOUT 168000u  /* the turn passed -> a GBA's start bit, at most */
#define MPB_NS_MASTER_HOLD  16820u   /* as parent: SC low after the last word */
#define MPB_NS_WORD         (18u * MPB_NS_BIT)  /* start + 16 + stop          */

#define MPB_MAX_PORTS 3
#define MPB_NO_WORD   0xFFFFu        /* a slot nobody answered               */

typedef struct {
    uint32_t (*now)(void *ctx);                    /* ticks                  */
    void     (*wait_until)(void *ctx, uint32_t t);
    bool     (*sd_get)(void *ctx);
    void     (*sd_out)(void *ctx, bool drive, bool level);   /* drive=false: let go */
    void     (*sc_out)(void *ctx, bool low);       /* parent only            */
    void     (*so_out)(void *ctx, bool high);
    bool     (*si_get)(void *ctx, int port);       /* port 1..3: that GBA's SO */
    void     *ctx;
    uint32_t ticks_per_us;
} mp_bus_hal_t;

typedef struct {
    uint8_t players;    /* on the link, 2..4                                 */
    uint8_t locals;     /* GBAs on this board's chain, ports 1..locals, 0..3 */
    bool    parent;     /* the board is slot 0 (a remote player is parent)   */
} mp_bus_layout_t;

/* Is the layout one the chain can carry: 2..4 players, 1..3 GBAs on the
 * chain, and at least one slot left for the board. */
bool mp_bus_layout_ok(const mp_bus_layout_t *l);

/* The slots the board answers, as a bit mask; the rest are local GBAs'. */
uint8_t mp_bus_board_slots(const mp_bus_layout_t *l);

/* The port a local slot's GBA is on (1..3), or 0 if the board answers it. */
int mp_bus_port_of(const mp_bus_layout_t *l, int slot);

typedef struct {
    uint32_t transfers, missed_slots, late_turns;
} mp_bus_stats_t;

/*
 * A local GBA is the parent: it dropped SC at t_sc_fall, in ticks (the caller's best
 * estimate: an interrupt's entry less its latency). Receive every word,
 * send ours (tx[slot] for the board's slots) when its turn comes, and
 * return every slot's word in rx (MPB_NO_WORD for a slot that never came).
 * The board's SO stays low throughout, which is what makes port 1's GBA the
 * parent.
 */
void mp_bus_child(const mp_bus_hal_t *h, const mp_bus_layout_t *l, uint32_t t_sc_fall,
                  const uint16_t tx[4], uint16_t rx[4], mp_bus_stats_t *st);

/*
 * The board is the parent: one whole transfer, SC and the first word
 * ours, then each local GBA's and the board's other slots in turn.
 */
void mp_bus_parent(const mp_bus_hal_t *h, const mp_bus_layout_t *l,
                   const uint16_t tx[4], uint16_t rx[4], mp_bus_stats_t *st);

/* At rest between transfers: SO low for a local parent (port 1's GBA sees
 * SI low), high when the board is parent; SD let go; SC let go or high. */
void mp_bus_idle(const mp_bus_hal_t *h, const mp_bus_layout_t *l);

#ifdef __cplusplus
}
#endif

#endif
