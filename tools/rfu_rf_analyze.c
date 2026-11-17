/*
 * rfu_rf_analyze.c -- what an ESP-SDR CAP20 snapshot holds, by src/rfu_rf.c.
 *
 *   build/rfu_rf_analyze CAPTURE.cap20 [--rate-code N] [--rate-hint BPS]
 *                        [--thomas] [--smooth N]
 *
 * The file is a payload tools/rfu_capture.py saved (or any CAP20 payload);
 * its sample count is the file's length. Prints the noise floor and peak,
 * clipped samples, and for each burst its time, length, AM levels and depth,
 * the recovered bit rate, the bits (hex, MSB first) and the Manchester
 * violations -- the RF-0/RF-1 measurements doc/RFU-RF.md lists. Nothing here
 * knows the RFU's air framing.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rfu_rf.h"

static const double k_msps[10] = { 80, 40, 20, 10, 8, 4, 16, 32, 10.667, 6.4 };

int main(int argc, char **argv) {
    const char *path = NULL;
    int rate_code = 0, thomas = 0, smooth = -1;
    double hint = 5e6;
    for (int k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "--rate-code") && k + 1 < argc) rate_code = atoi(argv[++k]);
        else if (!strcmp(argv[k], "--rate-hint") && k + 1 < argc) hint = atof(argv[++k]);
        else if (!strcmp(argv[k], "--thomas")) thomas = 1;
        else if (!strcmp(argv[k], "--smooth") && k + 1 < argc) smooth = atoi(argv[++k]);
        else if (argv[k][0] != '-' && !path) path = argv[k];
        else { fprintf(stderr, "usage: %s CAPTURE.cap20 [--rate-code N] [--rate-hint BPS] [--thomas] [--smooth N]\n", argv[0]); return 2; }
    }
    if (!path || rate_code < 0 || rate_code > 9) {
        fprintf(stderr, "usage: %s CAPTURE.cap20 [--rate-code 0..9] ...\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    const long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = malloc((size_t)bytes + 3);
    if (!raw || fread(raw, 1, (size_t)bytes, f) != (size_t)bytes) { fprintf(stderr, "%s: read failed\n", path); return 1; }
    fclose(f);
    raw[bytes] = raw[bytes + 1] = raw[bytes + 2] = 0;
    const size_t n = (size_t)bytes * 8 / 20;
    const double fs = k_msps[rate_code] * 1e6;

    int16_t *i = malloc(n * sizeof *i), *q = malloc(n * sizeof *q);
    uint32_t *p = malloc(n * sizeof *p), *env = malloc(n * sizeof *env), *work = malloc(n * sizeof *work);
    if (!i || !q || !p || !env || !work) { fprintf(stderr, "out of memory\n"); return 1; }
    rfrf_cap20_unpack(raw, n, i, q);
    const size_t clipped = rfrf_power(i, q, n, p);

    rfrf_cfg_t cfg;
    rfrf_cfg_default(&cfg, fs);
    cfg.rate_hint = hint;
    cfg.polarity = thomas ? RFRF_THOMAS : RFRF_IEEE;
    const double t0 = fs / (2 * hint), tmax = fs / (2 * cfg.rate_min);
    const unsigned width = smooth >= 1 ? (unsigned)smooth : t0 / 2 >= 1.5 ? (unsigned)lround(t0 / 2) : 1;
    rfrf_smooth(p, n, width, env);

    rfrf_span_t spans[64];
    rfrf_levels_t lv;
    const size_t nb = rfrf_find_bursts(env, n, (unsigned)(16 * t0), (unsigned)ceil(3 * tmax), work, spans, 64, &lv);
    printf("%s: %zu samples at %.3f MS/s (%.2f us), smoothing %u\n", path, n, fs / 1e6, n / fs * 1e6, width);
    printf("  noise floor %u, peak %u (power), threshold %u; clipped samples %zu%s\n", lv.floor, lv.peak,
           lv.threshold, clipped, clipped ? "  <- lower the gain" : "");
    printf("  %zu burst(s)\n", nb);
    uint8_t *bits = malloc(n / 8 + 16);
    for (size_t k = 0; k < nb; k++) {
        rfrf_burst_t b;
        const bool ok = rfrf_decode(env, spans[k], &cfg, work, bits, n / 8 + 16, &b);
        printf("  burst %zu: %.3f-%.3f us (%.2f us)  levels %u/%u  depth %.2f", k, spans[k].start / fs * 1e6,
               spans[k].end / fs * 1e6, (spans[k].end - spans[k].start) / fs * 1e6, b.lo, b.hi, b.depth);
        if (b.half_bit > 0)
            printf("  %.3f Mbit/s  lead %zu half-bits  %zu bits  %zu violations%s\n", b.rate / 1e6, b.lead,
                   b.nbits, b.violations, ok ? "" : "  (not a clean decode)");
        else
            printf("  no clock found\n");
        if (b.nbits) {
            printf("    ");
            for (size_t j = 0; j < (b.nbits + 7) / 8; j++) printf("%02X%s", bits[j], (j + 1) % 32 ? "" : "\n    ");
            printf("\n");
        }
    }
    free(bits); free(raw); free(i); free(q); free(p); free(env); free(work);
    return 0;
}
