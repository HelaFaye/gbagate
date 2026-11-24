/*
 * rl_codec.h -- GBA link waveforms as RMT symbols, and back: the part of the
 * RMT link test that needs no hardware, so the host tests it (test/).
 *
 * A symbol is the RMT's 32-bit word: duration0 (15 bits), level0, duration1
 * (15 bits), level1. Durations are in ticks of RL_RES_HZ.
 *
 * Multi-Player (SD): idle high; start bit low, 16 data bits least
 * significant first, stop bit high; 115200 baud, 8.68 us a bit (gbagate's
 * src/mp_bus.h). Word 0x0000 is the longest run: the start bit and 16 zero
 * bits, 17 bits low, 147.6 us -- longer than an RX idle threshold of 60 us.
 *
 * Normal mode (SC, SO): SC idles high and falls for each bit; SO changes on
 * SC's falling edge and is read on its rising edge; 32 bits, most
 * significant first; 256 kHz or 2 MHz.
 */
#ifndef RL_CODEC_H
#define RL_CODEC_H
#include <stdint.h>

#define RL_RES_HZ   40000000u           /* 25 ns ticks: 2 MHz bits are 20 */
#define RL_MP_NS    8680u               /* 115200 baud */
#define RL_MP_BITS  18                  /* start + 16 + stop */

static inline uint32_t rl_sym(int l0, uint32_t d0, int l1, uint32_t d1) {
    return (d0 & 0x7FFF) | (uint32_t)(l0 & 1) << 15 | (d1 & 0x7FFF) << 16 | (uint32_t)(l1 & 1) << 31;
}
static inline uint32_t rl_ticks(uint64_t ns) { return (uint32_t)((ns * RL_RES_HZ + 500000000ull) / 1000000000ull); }

/* A level held for a time: what an RX channel reports, in order. */
typedef struct { uint8_t level; uint32_t ticks; } rl_run_t;

/* Encode one Multi-Player word at bit_ns per bit: symbols for TX, ending with
 * an end marker (duration 0). Bit edges are placed at round(k * bit_ns), so
 * rounding never accumulates. Returns the symbol count, or -1 if out of room. */
int rl_mp_encode(uint16_t word, uint32_t bit_ns, uint32_t *sym, int max);
/* The longest run of one level an RX channel must time, in bits: every run
 * but the last high one (the stop bit, running on into the idle line). A
 * frame is cut short when this is longer than the RX idle threshold. */
int rl_mp_longest_run(uint16_t word);

/* Decode runs received from SD into a word. 0, or why not:
 * RL_ERR_START (no start bit), RL_ERR_SHORT (the frame ended early: the
 * capture stopped before 18 bits, as an RX idle threshold shorter than a run
 * makes it), RL_ERR_STOP (no stop bit), RL_ERR_TIMING (a run not a whole
 * number of bits, within a quarter bit). */
enum { RL_OK = 0, RL_ERR_START = -1, RL_ERR_SHORT = -2, RL_ERR_STOP = -3, RL_ERR_TIMING = -4 };
int rl_mp_decode(const rl_run_t *run, int n, uint32_t bit_ns, uint16_t *word);

/* Normal mode: SC and SO symbols for one 32-bit word, sent together (an RMT
 * sync manager starts both channels at once). Returns 0, or -1 if out of room. */
int rl_normal_encode(uint32_t word, uint32_t bit_ns, uint32_t *sc, int *n_sc, uint32_t *so, int *n_so, int max);
/* Decode: SO's level at each SC rising edge. Both captures start at their
 * first edge, so SO must have one at the start: bit 31 of the word is 0 (SO
 * falls with SC's first fall). Returns 0, or an RL_ERR. */
int rl_normal_decode(const rl_run_t *sc, int n_sc, const rl_run_t *so, int n_so, uint32_t *word);

/* What an RX channel would capture of a waveform given as runs: from the
 * first edge, ending at the first run longer than max_ns (the idle threshold).
 * For the host tests, and to predict the hardware's results. */
int rl_rx_model(const rl_run_t *in, int n, uint32_t max_ns, rl_run_t *out, int max_out);
/* Symbols to runs (as an RX channel's symbols are read), dropping zero-length halves. */
int rl_sym_to_runs(const uint32_t *sym, int n, rl_run_t *out, int max_out);
#endif
