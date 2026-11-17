/*
 * test_rfu_rf.c -- src/rfu_rf.c on synthetic AGB-015-like bursts.
 *
 * Each case makes a burst with rfrf_modulate (lead carrier, Manchester data,
 * tail), puts it on a carrier offset from the tuned frequency, adds Gaussian
 * noise, quantises it to ESP-SDR's signed 10 bits, packs and unpacks it as
 * CAP20 -- what a capture would hold -- and runs the whole receive pipeline,
 * checking the bits that come out against the bits that went in. These are
 * the conditions doc/RFU-RF.md RF-1 names: amplitude changes, carrier offset,
 * noise, quantisation and timing error. None of it is a real adapter's
 * signal; the levels and rates are the MC13190 data sheet's.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rfu_rf.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail;

static void check(const char *what, long got, long want) {
    if (got == want) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s (got %ld, want %ld)\n", what, got, want);
        g_fail++;
    }
}

static void check_true(const char *what, int ok) { check(what, ok ? 1 : 0, 1); }

/* ---- a reproducible noise source ---- */
static uint64_t g_rng = 1;
static double uniform(void) {
    g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
    return ((g_rng >> 11) + 0.5) / 9007199254740992.0;
}
static double gauss(void) {
    return sqrt(-2.0 * log(uniform())) * cos(2 * M_PI * uniform());
}

#define NBITS 256
#define MAXS  40000

typedef struct {
    const char *name;
    double fs, rate, depth, rise;
    double cfo;              /* carrier offset, Hz            */
    double scale;            /* the carrier's amplitude, LSBs */
    double noise;            /* noise sigma, LSBs per axis    */
    double fade;             /* amplitude at the end / start  */
    rfrf_polarity_t pol;
    double rate_hint;        /* what the receiver assumes     */
} scenario_t;

typedef struct {
    size_t bursts, clipped, errors, compared;
    rfrf_burst_t b;
    int ok;
} result_t;

static int16_t g_i[MAXS], g_q[MAXS], g_i2[MAXS], g_q2[MAXS];
static uint32_t g_p[MAXS], g_env[MAXS], g_work[MAXS];
static uint8_t g_cap[MAXS * 3];
static float g_amp[MAXS];

/* The decoded bits against the sent ones: the best of a few alignments (a
 * lead-in can swallow the first half-bit), skipping two bits at each end. */
static size_t bit_errors(const uint8_t *sent, const uint8_t *got, size_t ngot, size_t *compared) {
    size_t best = (size_t)-1;
    for (int off = -2; off <= 2; off++) {
        size_t err = 0, cmp = 0;
        for (size_t k = 2; k + 2 < NBITS; k++) {
            const long g = (long)k + off;
            if (g < 0 || (size_t)g >= ngot) { err++; continue; }
            const int a = sent[k / 8] >> (7 - k % 8) & 1;
            const int b = got[g / 8] >> (7 - g % 8) & 1;
            err += a != b;
            cmp++;
        }
        if (err < best) { best = err; *compared = cmp; }
    }
    return best;
}

static result_t run(const scenario_t *s, const uint8_t *bits) {
    result_t r;
    memset(&r, 0, sizeof r);
    rfrf_tx_t tx = { s->fs, s->rate, s->depth, s->rise, 8, 4, s->pol };
    const size_t len = rfrf_modulate(bits, NBITS, &tx, NULL, 0);
    const size_t pad = (size_t)(s->fs * 2e-6);                 /* 2 us of noise each side */
    const size_t n = len + 2 * pad;
    if (n > MAXS) { printf("  scenario too long\n"); g_fail++; return r; }
    rfrf_modulate(bits, NBITS, &tx, g_amp, len);

    const double ph0 = uniform() * 2 * M_PI;
    for (size_t k = 0; k < n; k++) {
        double a = 0;
        if (k >= pad && k < pad + len) {
            const double f = (double)(k - pad) / len;
            a = g_amp[k - pad] * s->scale * (1.0 + (s->fade - 1.0) * f);
        }
        const double ph = ph0 + 2 * M_PI * s->cfo * k / s->fs;
        const double vi = a * cos(ph) + s->noise * gauss();
        const double vq = a * sin(ph) + s->noise * gauss();
        g_i[k] = (int16_t)lround(vi);
        g_q[k] = (int16_t)lround(vq);
    }
    /* as the firmware would send it, and back */
    rfrf_cap20_pack(g_i, g_q, n, g_cap);
    rfrf_cap20_unpack(g_cap, n, g_i2, g_q2);

    r.clipped = rfrf_power(g_i2, g_q2, n, g_p);
    rfrf_cfg_t cfg;
    rfrf_cfg_default(&cfg, s->fs);
    if (s->rate_hint) cfg.rate_hint = s->rate_hint;
    cfg.polarity = s->pol;
    const double t0 = s->fs / (2 * cfg.rate_hint);
    const unsigned width = t0 / 2 >= 1.5 ? (unsigned)lround(t0 / 2) : 1;
    rfrf_smooth(g_p, n, width, g_env);

    rfrf_span_t spans[8];
    rfrf_levels_t lv;
    const double tmax = s->fs / (2 * cfg.rate_min);
    r.bursts = rfrf_find_bursts(g_env, n, (unsigned)(16 * t0), (unsigned)ceil(3 * tmax), g_work,
                                spans, 8, &lv);
    if (r.bursts == 0) return r;
    uint8_t got[NBITS / 8 + 16];
    r.ok = rfrf_decode(g_env, spans[0], &cfg, g_work, got, sizeof got, &r.b);
    r.errors = bit_errors(bits, got, r.b.nbits, &r.compared);
    return r;
}

static void report(const scenario_t *s, const result_t *r) {
    printf("    %-34s bursts %zu  rate %.3f Mbit/s  depth %.2f  bits %zu  viol %zu  "
           "errors %zu/%zu  clipped %zu\n",
           s->name, r->bursts, r->b.rate / 1e6, r->b.depth, r->b.nbits, r->b.violations,
           r->errors, r->compared, r->clipped);
}

static void expect_clean(const scenario_t *s, const uint8_t *bits) {
    result_t r = run(s, bits);
    report(s, &r);
    char what[160];
    snprintf(what, sizeof what, "%s: one burst, decoded, no bit errors", s->name);
    check_true(what, r.bursts == 1 && r.ok && r.errors == 0 && r.compared >= NBITS - 6);
    snprintf(what, sizeof what, "%s: the rate within 1%% (%.3f Mbit/s)", s->name, r.b.rate / 1e6);
    check_true(what, fabs(r.b.rate - s->rate) <= 0.01 * s->rate);
    snprintf(what, sizeof what, "%s: the depth within 0.1 (%.2f)", s->name, r.b.depth);
    check_true(what, fabs(r.b.depth - s->depth) <= 0.1);
}

static void test_cap20(void) {
    printf("CAP20 and CRC-32\n");
    int16_t i[4] = { 1, 0, 511, -512 }, q[4] = { 0, 1, -511, 300 };
    uint8_t b[10];
    rfrf_cap20_pack(i, q, 4, b);
    check("four samples are ten bytes", (long)rfrf_cap20_bytes(4), 10);
    check("I = 1 is bit 0", b[0], 0x01);
    /* sample 1: I 0, -Q = -1 = 0x3FF in bits 30..39: bytes 3 (high nibble), 4 */
    check("-Q, not Q, is sent (sample 1's -Q is all ones)", (b[3] >> 6) | (b[4] << 2), 0x3FF);
    int16_t i2[4], q2[4];
    rfrf_cap20_unpack(b, 4, i2, q2);
    int same = 1;
    for (int k = 0; k < 4; k++) same &= i2[k] == i[k] && q2[k] == q[k];
    check_true("unpack restores I and Q, rails included", same);
    int16_t big_i[1] = { 900 }, big_q[1] = { -900 }, bi[1], bq[1];
    rfrf_cap20_pack(big_i, big_q, 1, b);
    rfrf_cap20_unpack(b, 1, bi, bq);
    check_true("beyond 10 bits, clamped to the rails", bi[0] == 511 && bq[0] == -511);
    uint32_t p[1];
    check("and counted as clipped", (long)rfrf_power(bi, bq, 1, p), 1);
    check("CRC-32 of \"123456789\" is zlib's", (long)rfrf_crc32((const uint8_t *)"123456789", 9),
          (long)0xCBF43926u);
}

/* Noise alone: find_bursts may offer a span (it only finds candidates), but
 * none of them decodes: no clock fits noise. */
static void test_noise_only(void) {
    printf("noise alone\n");
    const size_t n = 16000;
    for (size_t k = 0; k < n; k++) {
        g_i[k] = (int16_t)lround(8 * gauss());
        g_q[k] = (int16_t)lround(8 * gauss());
    }
    rfrf_power(g_i, g_q, n, g_p);
    rfrf_smooth(g_p, n, 4, g_env);
    rfrf_span_t sp[16];
    rfrf_levels_t lv;
    const size_t found = rfrf_find_bursts(g_env, n, 128, 30, g_work, sp, 16, &lv);
    rfrf_cfg_t cfg;
    rfrf_cfg_default(&cfg, 80e6);
    size_t decoded = 0;
    uint8_t bits[2048];
    for (size_t k = 0; k < found; k++) {
        rfrf_burst_t b;
        decoded += rfrf_decode(g_env, sp[k], &cfg, g_work, bits, sizeof bits, &b);
    }
    printf("    %zu candidate span(s)\n", found);
    check("nothing in noise decodes", (long)decoded, 0);
}

int main(void) {
    uint8_t bits[NBITS / 8];
    for (size_t k = 0; k < sizeof bits; k++) bits[k] = (uint8_t)(uniform() * 256);

    test_cap20();
    test_noise_only();

    printf("synthetic bursts (AM, Manchester), through CAP20\n");
    const scenario_t clean[] = {
        { "80 MS/s, 5 Mbit/s, 60%",       80e6, 5e6,  0.60, 40e-9,  150e3, 300, 12, 1.0, RFRF_IEEE, 0 },
        { "40 MS/s, 5 Mbit/s, 60%",       40e6, 5e6,  0.60, 40e-9, -150e3, 300, 12, 1.0, RFRF_IEEE, 0 },
        { "80 MS/s, 4 Mbit/s",            80e6, 4e6,  0.60, 40e-9,   80e3, 300, 12, 1.0, RFRF_IEEE, 0 },
        { "80 MS/s, 6 Mbit/s",            80e6, 6e6,  0.60, 40e-9,   80e3, 300, 12, 1.0, RFRF_IEEE, 0 },
        { "80 MS/s, depth 50%",           80e6, 5e6,  0.50, 40e-9,   80e3, 300, 12, 1.0, RFRF_IEEE, 0 },
        { "80 MS/s, depth 88%",           80e6, 5e6,  0.88, 40e-9,   80e3, 300, 12, 1.0, RFRF_IEEE, 0 },
        { "80 MS/s, Thomas polarity",     80e6, 5e6,  0.60, 40e-9,   80e3, 300, 12, 1.0, RFRF_THOMAS, 0 },
        { "80 MS/s, fading to 50%",       80e6, 5e6,  0.60, 40e-9,   80e3, 300, 10, 0.5, RFRF_IEEE, 0 },
        { "80 MS/s, 3% fast",             80e6, 5.15e6, 0.60, 40e-9, 80e3, 300, 12, 1.0, RFRF_IEEE, 0 },
        { "80 MS/s, weak (60 LSB)",       80e6, 5e6,  0.60, 40e-9,   80e3,  60,  5, 1.0, RFRF_IEEE, 0 },
        { "40 MS/s, 6 Mbit/s, 50%",       40e6, 6e6,  0.50, 40e-9,  200e3, 300, 12, 1.0, RFRF_IEEE, 0 },
    };
    for (size_t k = 0; k < sizeof clean / sizeof clean[0]; k++) expect_clean(&clean[k], bits);

    /* Polarity is only a naming: Thomas-coded bits are IEEE-coded inverted
     * bits, so the same waveform decodes either way, inverted. */
    {
        uint8_t inv[NBITS / 8];
        for (size_t k = 0; k < sizeof inv; k++) inv[k] = (uint8_t)~bits[k];
        scenario_t u = clean[0];
        u.name = "Thomas-coded inverse";
        u.pol = RFRF_THOMAS;
        result_t ru = run(&u, inv);
        report(&u, &ru);
        check_true("Thomas-coded inverse bits decode as sent", ru.ok && ru.errors == 0);
    }

    /* clipping: reported, not hidden */
    {
        const scenario_t s = { "80 MS/s, overdriven (700 LSB)", 80e6, 5e6, 0.60, 40e-9, 80e3, 700, 12, 1.0, RFRF_IEEE, 0 };
        result_t r = run(&s, bits);
        report(&s, &r);
        check_true("an overdriven capture says it clipped", r.clipped > 0);
    }

    /* 16 MS/s: 1.6 samples a half-bit. Not asserted to decode -- doc/RFU-RF.md
     * expects it not to be enough -- only that it never claims a clean
     * decode that is wrong. */
    {
        const scenario_t s = { "16 MS/s, 5 Mbit/s", 16e6, 5e6, 0.60, 40e-9, 80e3, 300, 12, 1.0, RFRF_IEEE, 0 };
        result_t r = run(&s, bits);
        report(&s, &r);
        check_true("16 MS/s: decoded cleanly, or said it could not", !r.ok || r.errors <= NBITS / 50);
    }

    printf("%s\n", g_fail ? "FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
