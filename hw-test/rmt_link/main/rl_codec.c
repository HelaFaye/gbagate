/* rl_codec.c -- see rl_codec.h. */
#include "rl_codec.h"

/* Levels for each bit of a Multi-Player frame. */
static void mp_levels(uint16_t word, uint8_t bit[RL_MP_BITS]) {
    bit[0] = 0;
    for (int k = 0; k < 16; k++) bit[1 + k] = (uint8_t)(word >> k & 1);
    bit[17] = 1;
}

int rl_mp_longest_run(uint16_t word) {
    /* the runs an RX channel must time: every one before the last high run,
     * which is the stop bit running on into the idle line */
    uint8_t b[RL_MP_BITS];
    mp_levels(word, b);
    int end = RL_MP_BITS;
    while (end > 0 && b[end - 1]) end--;
    int best = 0, run = 0;
    for (int k = 0; k < end; k++) {
        run = (k && b[k] == b[k - 1]) ? run + 1 : 1;
        if (run > best) best = run;
    }
    return best;
}

/* Runs to TX symbols, two halves to a symbol, then an end marker. */
static int pack(const rl_run_t *run, int n, uint32_t *sym, int max) {
    int s = 0;
    for (int k = 0; k < n; k += 2) {
        if (s >= max) return -1;
        const rl_run_t *b = k + 1 < n ? &run[k + 1] : 0;
        sym[s++] = rl_sym(run[k].level, run[k].ticks, b ? b->level : run[k].level, b ? b->ticks : 0);
    }
    if (n % 2 == 0) {                   /* the end marker: a zero duration */
        if (s >= max) return -1;
        sym[s++] = rl_sym(run[n - 1].level, 0, run[n - 1].level, 0);
    }
    return s;
}

int rl_mp_encode(uint16_t word, uint32_t bit_ns, uint32_t *sym, int max) {
    uint8_t b[RL_MP_BITS];
    mp_levels(word, b);
    rl_run_t run[RL_MP_BITS];
    int n = 0, start = 0;
    for (int k = 1; k <= RL_MP_BITS; k++) {
        if (k == RL_MP_BITS || b[k] != b[start]) {
            /* edges at round(k * bit), so rounding never adds up */
            run[n].level = b[start];
            run[n].ticks = rl_ticks((uint64_t)k * bit_ns) - rl_ticks((uint64_t)start * bit_ns);
            n++;
            start = k;
        }
    }
    return pack(run, n, sym, max);
}

int rl_mp_decode(const rl_run_t *run, int n, uint32_t bit_ns, uint16_t *word) {
    const uint32_t bit = rl_ticks(bit_ns);
    int k = 0;
    while (k < n && run[k].level) k++;          /* to the start bit's fall */
    if (k == n) return RL_ERR_START;
    uint8_t b[RL_MP_BITS];
    int got = 0;
    for (; k < n && got < RL_MP_BITS; k++) {
        const uint32_t t = run[k].ticks;
        uint32_t bits;
        if (t == 0) {
            /* the capture's end: a high level is the stop bit running on into
             * the idle line, untimed; a low one is a run cut short */
            if (!run[k].level) break;
            bits = (uint32_t)(RL_MP_BITS - got);
        } else {
            bits = (t + bit / 2) / bit;
            const uint32_t off = t > bits * bit ? t - bits * bit : bits * bit - t;
            if (off > bit / 4 || bits == 0) return RL_ERR_TIMING;
        }
        for (uint32_t j = 0; j < bits && got < RL_MP_BITS; j++) b[got++] = run[k].level;
    }
    if (got < RL_MP_BITS) return RL_ERR_SHORT;
    if (b[0] != 0) return RL_ERR_START;
    if (b[17] != 1) return RL_ERR_STOP;
    uint16_t w = 0;
    for (int j = 0; j < 16; j++) w |= (uint16_t)(b[1 + j] << j);
    *word = w;
    return RL_OK;
}

int rl_normal_encode(uint32_t word, uint32_t bit_ns, uint32_t *sc, int *n_sc, uint32_t *so, int *n_so, int max) {
    rl_run_t c[64], d[33];
    int nc = 0, nd = 0;
    for (int k = 0; k < 32; k++) {
        const uint32_t t0 = rl_ticks((uint64_t)k * bit_ns), tm = rl_ticks((uint64_t)k * bit_ns + bit_ns / 2),
                       t1 = rl_ticks((uint64_t)(k + 1) * bit_ns);
        c[nc++] = (rl_run_t){0, tm - t0};       /* SC low: SO changes here */
        c[nc++] = (rl_run_t){1, t1 - tm};       /* SC high: the GBA reads SO at the rise */
        const uint8_t level = (uint8_t)(word >> (31 - k) & 1);
        if (nd && d[nd - 1].level == level) d[nd - 1].ticks += t1 - t0;
        else d[nd++] = (rl_run_t){level, t1 - t0};
    }
    d[nd++] = (rl_run_t){1, rl_ticks(bit_ns)};  /* back to idle high */
    *n_sc = pack(c, nc, sc, max);
    *n_so = pack(d, nd, so, max);
    return *n_sc < 0 || *n_so < 0 ? -1 : 0;
}

/* The level of a capture at time t (ticks from its first edge); the last
 * level holds after its end. */
static int level_at(const rl_run_t *r, int n, uint64_t t) {
    uint64_t at = 0;
    for (int k = 0; k < n; k++) {
        if (r[k].ticks == 0) return r[k].level;
        if (t < at + r[k].ticks) return r[k].level;
        at += r[k].ticks;
    }
    return n ? r[n - 1].level : 1;
}

int rl_normal_decode(const rl_run_t *sc, int n_sc, const rl_run_t *so, int n_so, uint32_t *word) {
    /* each capture from its first fall: an idle high before it is dropped,
     * however the channel started (SC and SO fall together, sent in sync) */
    while (n_sc && sc->level && sc->ticks) { sc++; n_sc--; }
    while (n_so && so->level && so->ticks) { so++; n_so--; }
    uint64_t t = 0;
    uint32_t w = 0;
    int bits = 0;
    for (int k = 0; k < n_sc && bits < 32; k++) {
        if (sc[k].ticks == 0) break;
        t += sc[k].ticks;
        if (sc[k].level == 0) {                 /* the end of a low run: SC rises here */
            w = w << 1 | (uint32_t)level_at(so, n_so, t);
            bits++;
        }
    }
    if (bits < 32) return RL_ERR_SHORT;
    *word = w;
    return RL_OK;
}

int rl_rx_model(const rl_run_t *in, int n, uint32_t max_ns, rl_run_t *out, int max_out) {
    const uint32_t idle = rl_ticks(max_ns);
    int k = 0, m = 0;
    if (n && in[0].level == 1) k = 1;           /* idle high before the first edge: not captured */
    for (; k < n && m < max_out; k++) {
        if (in[k].ticks > idle) {               /* no edge for longer than the threshold: done */
            out[m++] = (rl_run_t){in[k].level, 0};
            return m;
        }
        out[m++] = in[k];
    }
    if (m < max_out) out[m++] = (rl_run_t){n ? in[n - 1].level : 1, 0};
    return m;
}

int rl_sym_to_runs(const uint32_t *sym, int n, rl_run_t *out, int max_out) {
    int m = 0;
    for (int k = 0; k < n && m < max_out; k++) {
        const uint32_t d0 = sym[k] & 0x7FFF, d1 = sym[k] >> 16 & 0x7FFF;
        const uint8_t l0 = sym[k] >> 15 & 1, l1 = (uint8_t)(sym[k] >> 31);
        out[m++] = (rl_run_t){l0, d0};
        if (d0 == 0) return m;
        if (m < max_out) out[m++] = (rl_run_t){l1, d1};
        if (d1 == 0) return m;
    }
    return m;
}
