/*
 * rfu_rf.h -- a portable baseband for the AGB-015's radio (doc/RFU-RF.md, RF-1).
 *
 * The Wireless Adapter's MC13190 sends Manchester-coded data, 4-6 Mbit/s
 * (5 typical), as AM on a 2.4 GHz carrier (2432 MHz nominal, 50-88% depth,
 * 60% typical). This file turns raw complex samples of such a signal -- an
 * ESP-SDR CAP20 snapshot from an ESP32-S3, at 80, 40 or 16 MS/s -- into
 * bursts and bits, and makes the envelope of a burst to send. Nothing in it
 * knows the RFU's air framing, which is still unknown: it stops at bits.
 *
 * No ESP-IDF, no allocation: every buffer is the caller's, so the same code
 * runs on the host (tools/rfu_rf_analyze, test/test_rfu_rf.c) and could later
 * run on a board. Nothing here has been checked against a real adapter yet.
 *
 * The pipeline, for one capture:
 *   rfrf_cap20_unpack   CAP20 bytes -> (I, Q), Q's sign restored
 *   rfrf_power          I^2 + Q^2 (no square root), and clipped samples
 *   rfrf_smooth         a box filter about a quarter bit long
 *   rfrf_find_bursts    where the carrier is on, against the noise floor
 *   rfrf_decode         per burst: the two AM levels, the slice between them,
 *                       the half-bit clock recovered from the edges, and the
 *                       Manchester bits, with what did not fit counted
 */
#ifndef RFU_RF_H
#define RFU_RF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ESP-SDR CAP20 ----------------------------------------------------------
 * Each complex sample is 20 bits, packed consecutively LSB first: bits 0-9 I,
 * 10-19 -Q, both signed 10-bit (ESP-SDR docs/protocol.md). The firmware sends
 * -Q; unpack negates it back. A sample at either rail (-512 or +511 in I or
 * -Q) is clipped. */
#define RFRF_CAP20_MAX  511
#define RFRF_CAP20_MIN  (-512)

size_t   rfrf_cap20_bytes(size_t samples);
void     rfrf_cap20_unpack(const uint8_t *in, size_t samples, int16_t *i, int16_t *q);
/* The inverse, for tests and synthetic files: (I, Q) in, (I, -Q) packed;
 * values beyond 10 bits are clamped to the rails. */
void     rfrf_cap20_pack(const int16_t *i, const int16_t *q, size_t samples, uint8_t *out);
/* zlib's CRC-32, as the DATA line carries. */
uint32_t rfrf_crc32(const uint8_t *data, size_t len);

/* ---- envelope ------------------------------------------------------------- */

/* I^2 + Q^2 into p; returns how many samples sat on a rail. */
size_t   rfrf_power(const int16_t *i, const int16_t *q, size_t n, uint32_t *p);
/* A centred box filter `width` samples wide (1: a copy). */
void     rfrf_smooth(const uint32_t *p, size_t n, unsigned width, uint32_t *out);

typedef struct {
    size_t   start, end;       /* [start, end) in samples */
} rfrf_span_t;

typedef struct {
    uint32_t floor;            /* the noise: 10th percentile of the envelope */
    uint32_t peak;             /* 99th percentile */
    uint32_t threshold;        /* what counts as carrier */
} rfrf_levels_t;

/*
 * Bursts: where the envelope stays above a threshold set between the noise
 * floor and the peak, at least min_len samples long, with dips shorter than
 * `gap` samples bridged (an AM low is still carrier). work: n uint32s, for
 * the percentiles. Returns the bursts found (at most max into out); a burst
 * still on at the end of the capture is reported, ending there.
 */
size_t   rfrf_find_bursts(const uint32_t *env, size_t n, unsigned min_len, unsigned gap,
                          uint32_t *work, rfrf_span_t *out, size_t max, rfrf_levels_t *lv);

/* ---- Manchester ----------------------------------------------------------- */

typedef enum {
    RFRF_IEEE = 0,             /* a 0 is high then low, a 1 low then high (802.3) */
    RFRF_THOMAS = 1,           /* the reverse (G. E. Thomas)                      */
} rfrf_polarity_t;

typedef struct {
    double fs;                 /* samples per second                              */
    double rate_hint;          /* bits per second to start from (5e6)             */
    double rate_min, rate_max; /* what the estimate may come out as (4e6, 6e6)    */
    rfrf_polarity_t polarity;
} rfrf_cfg_t;

void rfrf_cfg_default(rfrf_cfg_t *cfg, double fs);

typedef struct {
    rfrf_span_t span;
    uint32_t lo, hi;           /* the two AM levels (power), from the burst       */
    uint32_t slice;            /* between them                                    */
    double   depth;            /* 1 - sqrt(lo / hi): the AM depth                 */
    double   half_bit;         /* samples per half-bit, recovered                 */
    double   rate;             /* bits per second it implies                      */
    size_t   lead;             /* half-bits of steady level before the first bit  */
    size_t   nbits;            /* bits decoded (bits[], MSB of byte 0 first)      */
    size_t   violations;       /* bit cells with no mid-bit transition            */
    unsigned pairing;          /* which half-bits pair up (0 or 1)                */
    bool     ok;               /* a clock was found and the bits are mostly sound */
} rfrf_burst_t;

/*
 * Decode one burst of env (the smoothed power) between span.start and
 * span.end. bits: bits_cap bytes, written MSB first; work: (span length)
 * uint32s. A bit cell whose halves agree (a violation: no mid-bit
 * transition) is written as 0 and counted. ok is false when no half-bit
 * clock within cfg's rates fits the edges, or more than an eighth of the
 * cells are violations.
 */
bool rfrf_decode(const uint32_t *env, rfrf_span_t span, const rfrf_cfg_t *cfg,
                 uint32_t *work, uint8_t *bits, size_t bits_cap, rfrf_burst_t *out);

/* ---- transmit ------------------------------------------------------------- */

typedef struct {
    double fs, rate;
    double depth;              /* 0.6: the low level is 1 - depth of the high     */
    double rise;               /* 10-90% edge time in seconds (0: square)         */
    unsigned lead_bits;        /* unmodulated carrier before the data             */
    unsigned tail_bits;        /* ... and after it                                */
    rfrf_polarity_t polarity;
} rfrf_tx_t;

/*
 * The amplitude envelope (0..1) of a burst carrying nbits bits (MSB of
 * bits[0] first): lead_bits of carrier, the Manchester data at tx->rate with
 * the low level at 1 - depth, tail_bits of carrier, then a few samples of
 * silence for the edge filter to settle. Returns the samples it needs;
 * writes at most cap of them to amp (NULL: count only).
 */
size_t rfrf_modulate(const uint8_t *bits, size_t nbits, const rfrf_tx_t *tx, float *amp, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* RFU_RF_H */
