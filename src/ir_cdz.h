/*
 * ir_cdz.h -- the Cyber Drive Zoids infrared protocol.
 *
 * Source: Shonumi, "Edge of Emulation: GBA Infrared Adapter & Zoids Cyber
 * Drive" (2020), which is the only description of the AGB-006 that exists.
 * Everything below is his reverse engineering; what is ours is the
 * implementation and the tolerances.
 *
 * The one thing worth restating because it is easy to get backwards: pulses
 * are classified by **the gap before them**, not by how long the light is on.
 * The article is explicit -- the LED is on only briefly every time, and it is
 * the off period that varies. A decoder that measures ON duration sees four
 * identical pulses and never decodes anything.
 *
 *   Mini    ~18.1-20 us      keep-alive, carries no data
 *   Short   ~16x Mini        a 0 bit
 *   Medium  ~40x Mini        a 1 bit
 *   Long    ~6000x Mini      end of command
 *
 * A command is twelve Short/Medium pulses then one Long: a 12-bit value, MSB
 * first, split into three 4-bit fields. Each command has two forms, one per IR
 * channel, so two models can be driven side by side.
 *
 * No hardware here. The GBA side drives an LED off RCNT bit 3 (SO) in General
 * Purpose mode; port/esp32s3 does that, this decides what to send and what an
 * arriving train of pulses meant.
 */

#ifndef IR_CDZ_H
#define IR_CDZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Nominal gaps, microseconds. */
#define CDZ_MINI_US    19u
#define CDZ_SHORT_US   (CDZ_MINI_US * 16u)     /*  ~304 us */
#define CDZ_MEDIUM_US  (CDZ_MINI_US * 40u)     /*  ~760 us */
#define CDZ_LONG_US    (CDZ_MINI_US * 6000u)   /* ~114 ms  */

/*
 * Thresholds sit at the geometric mean of adjacent classes rather than the
 * arithmetic one. The classes are separated by ratios (16x, 2.5x, 150x), so a
 * midpoint in the log domain gives each the same proportional margin --
 * an arithmetic midpoint between 304 and 760 leaves Short with far more
 * tolerance than Medium, and Medium is the one a slow interrupt lengthens.
 */
#define CDZ_MINI_MAX_US    76u     /* sqrt(19 * 304)   ~76  */
#define CDZ_SHORT_MAX_US   481u    /* sqrt(304 * 760)  ~481 */
#define CDZ_MEDIUM_MAX_US  9300u   /* sqrt(760 * 114000) ~9308 */

#define CDZ_CMD_BITS 12

typedef enum {
    CDZ_PULSE_MINI = 0,
    CDZ_PULSE_SHORT,
    CDZ_PULSE_MEDIUM,
    CDZ_PULSE_LONG,
    CDZ_PULSE_INVALID
} cdz_pulse_t;

/* Classify one inter-pulse gap. */
cdz_pulse_t cdz_classify(uint32_t gap_us);

const char *cdz_pulse_name(cdz_pulse_t p);

/* ---- decoding ----------------------------------------------------------- */

typedef struct {
    uint32_t last_us;
    bool     have_last;
    uint16_t bits;
    uint8_t  count;
    uint32_t commands;   /* successfully decoded */
    uint32_t aborted;    /* trains that ended at the wrong length */
    uint32_t minis;
} cdz_rx_t;

void cdz_rx_init(cdz_rx_t *rx);

/*
 * Feed the timestamp of a light-on edge. Returns true and sets *cmd when a
 * Long pulse closes a complete twelve-bit command.
 *
 * The AGB-006 only interrupts on no-light to light, never the reverse, so an
 * edge timestamp is all the hardware can give us -- which is why this takes
 * times rather than durations.
 */
bool cdz_rx_edge(cdz_rx_t *rx, uint32_t now_us, uint16_t *cmd);

/* ---- command fields ------------------------------------------------------ */

/*
 * Three 4-bit fields. The article calls the split "not entirely clean" and
 * notes duplicates and conditional commands, so these are a reading aid rather
 * than a complete grammar -- treat an unrecognised combination as unknown, not
 * as an error.
 */
static inline uint8_t cdz_category(uint16_t cmd) { return (cmd >> 8) & 0xF; }
static inline uint8_t cdz_motion(uint16_t cmd)   { return (cmd >> 4) & 0xF; }
static inline uint8_t cdz_action(uint16_t cmd)   { return cmd & 0xF; }

/* ---- encoding ------------------------------------------------------------ */

/*
 * Expand a command into the gaps to wait before each light-on edge. Writes
 * CDZ_CMD_BITS + 1 entries: twelve bit gaps then the Long terminator. Returns
 * the count, or 0 if the buffer is too small.
 */
size_t cdz_encode(uint16_t cmd, uint32_t *gaps_us, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* IR_CDZ_H */
