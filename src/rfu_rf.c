/*
 * rfu_rf.c -- see rfu_rf.h.
 */
#include "rfu_rf.h"

#include <math.h>
#include <string.h>

/* ---- CAP20 ------------------------------------------------------------------ */

size_t rfrf_cap20_bytes(size_t samples) {
    return (samples * 20u + 7u) / 8u;
}

static int16_t sext10(uint32_t v) {
    v &= 0x3FFu;
    return (int16_t)(v & 0x200u ? (int32_t)v - 1024 : (int32_t)v);
}

void rfrf_cap20_unpack(const uint8_t *in, size_t samples, int16_t *i, int16_t *q) {
    for (size_t k = 0; k < samples; k++) {
        const size_t bit = k * 20u;
        const size_t byte = bit / 8u;
        const unsigned sh = (unsigned)(bit % 8u);   /* 0 or 4 */
        /* 20 bits from at most 3 bytes past `byte` (sh + 20 <= 24) */
        uint32_t w = (uint32_t)in[byte] | (uint32_t)in[byte + 1] << 8;
        if (sh + 20u > 16u) w |= (uint32_t)in[byte + 2] << 16;
        w >>= sh;
        i[k] = sext10(w);
        q[k] = (int16_t)-sext10(w >> 10);           /* the firmware sends -Q */
    }
}

static uint32_t clamp10(int v) {
    if (v > RFRF_CAP20_MAX) v = RFRF_CAP20_MAX;
    if (v < RFRF_CAP20_MIN) v = RFRF_CAP20_MIN;
    return (uint32_t)v & 0x3FFu;
}

void rfrf_cap20_pack(const int16_t *i, const int16_t *q, size_t samples, uint8_t *out) {
    memset(out, 0, rfrf_cap20_bytes(samples));
    for (size_t k = 0; k < samples; k++) {
        const uint32_t w = clamp10(i[k]) | clamp10(-q[k]) << 10;
        const size_t bit = k * 20u;
        const size_t byte = bit / 8u;
        const unsigned sh = (unsigned)(bit % 8u);
        const uint32_t v = w << sh;
        out[byte] |= (uint8_t)v;
        out[byte + 1] |= (uint8_t)(v >> 8);
        if (sh + 20u > 16u) out[byte + 2] |= (uint8_t)(v >> 16);
    }
}

uint32_t rfrf_crc32(const uint8_t *data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t k = 0; k < len; k++) {
        c ^= data[k];
        for (int b = 0; b < 8; b++) c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

/* ---- envelope ----------------------------------------------------------------- */

size_t rfrf_power(const int16_t *i, const int16_t *q, size_t n, uint32_t *p) {
    size_t clipped = 0;
    for (size_t k = 0; k < n; k++) {
        const int32_t a = i[k], b = q[k];
        p[k] = (uint32_t)(a * a + b * b);
        /* -Q at a rail is Q at the other (or one past it, for -512) */
        if (a >= RFRF_CAP20_MAX || a <= RFRF_CAP20_MIN || b >= -RFRF_CAP20_MIN || b <= -RFRF_CAP20_MAX)
            clipped++;
    }
    return clipped;
}

void rfrf_smooth(const uint32_t *p, size_t n, unsigned width, uint32_t *out) {
    if (width <= 1 || n == 0) {
        if (out != p) memcpy(out, p, n * sizeof *p);
        return;
    }
    const size_t before = (width - 1) / 2, after = width / 2;
    uint64_t sum = 0;
    size_t lo = 0, hi = 0;   /* the window is [lo, hi) */
    for (size_t k = 0; k < n; k++) {
        const size_t want_lo = k > before ? k - before : 0;
        const size_t want_hi = k + after + 1 < n ? k + after + 1 : n;
        while (hi < want_hi) sum += p[hi++];
        while (lo < want_lo) sum -= p[lo++];
        out[k] = (uint32_t)(sum / (hi - lo));
    }
}

/* The k-th smallest of a[0..n) (reorders a). */
static uint32_t select_kth(uint32_t *a, size_t n, size_t k) {
    size_t lo = 0, hi = n - 1;
    while (lo < hi) {
        const uint32_t pivot = a[lo + (hi - lo) / 2];
        size_t i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i <= j) {
                const uint32_t t = a[i]; a[i] = a[j]; a[j] = t;
                i++;
                if (j == 0) break;
                j--;
            }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else return a[k];
    }
    return a[k];
}

static uint32_t percentile(const uint32_t *v, size_t n, uint32_t *work, unsigned pct) {
    memcpy(work, v, n * sizeof *v);
    return select_kth(work, n, (n - 1) * pct / 100u);
}

size_t rfrf_find_bursts(const uint32_t *env, size_t n, unsigned min_len, unsigned gap,
                        uint32_t *work, rfrf_span_t *out, size_t max, rfrf_levels_t *lv) {
    rfrf_levels_t l = { 0, 0, 0 };
    if (n == 0) {
        if (lv) *lv = l;
        return 0;
    }
    l.floor = percentile(env, n, work, 10);
    l.peak = percentile(env, n, work, 99);
    l.threshold = l.floor + (l.peak - l.floor) / 10u;
    if (lv) *lv = l;
    if (l.peak <= l.floor + l.floor / 2u + 4u) return 0;   /* nothing above the noise */

    size_t found = 0, start = 0, below = 0;
    bool on = false;
    for (size_t k = 0; k <= n; k++) {
        const bool high = k < n && env[k] > l.threshold;
        if (!on) {
            if (high) { on = true; start = k; below = 0; }
            continue;
        }
        if (high) { below = 0; continue; }
        if (k < n && ++below <= gap) continue;           /* an AM low: still carrier */
        const size_t end = k < n ? k - below + 1 : k - below;
        if (end - start >= min_len) {
            if (found < max) out[found] = (rfrf_span_t){ start, end };
            found++;
        }
        on = false;
    }
    return found < max ? found : max;
}

/* ---- Manchester ----------------------------------------------------------------- */

void rfrf_cfg_default(rfrf_cfg_t *cfg, double fs) {
    cfg->fs = fs;
    cfg->rate_hint = 5e6;
    cfg->rate_min = 4e6;
    cfg->rate_max = 6e6;
    cfg->polarity = RFRF_IEEE;
}

/*
 * Edges, as positions in 1/256 sample, found with hysteresis around the
 * slice and placed where the envelope crossed the slice itself.
 */
/*
 * The slice at k: midway between the highest and lowest envelope within
 * `half` samples either side (Manchester puts both levels in any two bits),
 * so it follows a fading carrier. Where nothing in the window falls below the
 * burst's own slice (the lead-in and tail: steady carrier, only noise on it),
 * or the window's spread is under an eighth of the burst's contrast, the
 * burst's slice stands: a slice in the middle of noise would make edges of it.
 */
static void local_slice(const uint32_t *env, rfrf_span_t sp, size_t k, size_t half,
                        uint32_t slice, uint32_t contrast, uint32_t *s, uint32_t *h) {
    const size_t a = k > sp.start + half ? k - half : sp.start;
    const size_t b = k + half + 1 < sp.end ? k + half + 1 : sp.end;
    uint32_t mn = UINT32_MAX, mx = 0;
    for (size_t j = a; j < b; j++) {
        if (env[j] < mn) mn = env[j];
        if (env[j] > mx) mx = env[j];
    }
    if (mn >= slice || mx - mn < contrast / 8u) { *s = slice; *h = contrast / 8u; return; }
    *s = mn + (mx - mn) / 2u;
    *h = (mx - mn) / 8u;
}

static size_t find_edges(const uint32_t *env, rfrf_span_t sp, uint32_t slice, uint32_t contrast,
                         size_t half, uint32_t *edge, size_t cap, bool *first_level) {
    bool level = env[sp.start] >= slice;
    *first_level = level;
    size_t ne = 0;
    for (size_t k = sp.start + 1; k < sp.end; k++) {
        uint32_t sl, hyst;
        local_slice(env, sp, k, half, slice, contrast, &sl, &hyst);
        const uint32_t up = sl + hyst, down = sl > hyst ? sl - hyst : 0;
        const bool flip = level ? env[k] < down : env[k] > up;
        if (!flip) continue;
        level = !level;
        slice = sl;
        /* back to where it crossed the slice */
        size_t j = k;
        while (j > sp.start + 1 && (level ? env[j - 1] >= slice : env[j - 1] < slice)) j--;
        const double a = env[j - 1], b = env[j];
        double frac = b != a ? ((double)slice - a) / (b - a) : 0.5;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        const double x = (double)(j - 1) + frac - (double)sp.start;
        if (ne < cap) edge[ne++] = (uint32_t)lround(x * 256.0);
    }
    return ne;
}

/* The half-bit length that best explains the runs between edges, from T0. */
static double fit_half_bit(const uint32_t *edge, size_t ne, double t0, size_t *inliers) {
    double t = t0;
    size_t in = 0;
    for (int it = 0; it < 6; it++) {
        double sum_r = 0, sum_k = 0;
        in = 0;
        for (size_t j = 1; j < ne; j++) {
            const double r = (edge[j] - edge[j - 1]) / 256.0;
            const double k = floor(r / t + 0.5);
            if (k < 1 || k > 2 || fabs(r - k * t) > 0.35 * t) continue;
            sum_r += r;
            sum_k += k;
            in++;
        }
        if (sum_k == 0) break;
        t = sum_r / sum_k;
    }
    *inliers = in;
    return t;
}

/* Walks the half-bit cells the edges imply; counts violations per pairing,
 * and (when bits) writes the bits of `pairing`. */
typedef struct {
    int      prev[2];            /* the first half of a pending pair, -1 none */
    size_t   cells;
    size_t   viol[2], pairs[2];
    uint8_t *bits;
    size_t   cap, nbits;
    unsigned pairing;
    rfrf_polarity_t pol;
} cells_t;

static void cell(cells_t *c, int level) {
    for (unsigned p = 0; p < 2; p++) {
        /* pairing p pairs cells (p, p+1), (p+2, p+3), ... */
        if (c->cells < p) continue;
        if (((c->cells - p) & 1u) == 0) {
            c->prev[p] = level;
            continue;
        }
        const int a = c->prev[p];
        c->pairs[p]++;
        if (a == level) c->viol[p]++;
        if (c->bits && p == c->pairing) {
            /* IEEE: high-low is 0, low-high 1; Thomas the reverse */
            int bit = a == level ? 0 : (a < level);
            if (c->pol == RFRF_THOMAS && a != level) bit = !bit;
            if (c->nbits / 8 < c->cap) {
                uint8_t *by = &c->bits[c->nbits / 8];
                if (bit) *by |= (uint8_t)(0x80u >> (c->nbits % 8));
                else *by &= (uint8_t)~(0x80u >> (c->nbits % 8));
            }
            c->nbits++;
        }
    }
    c->cells++;
}

static void walk(const uint32_t *edge, size_t ne, double t, bool level0, cells_t *c) {
    /* the first edge starts the first data cell; before it is lead-in */
    double b = edge[0] / 256.0;
    bool level = !level0;            /* the level after the first edge */
    for (size_t j = 1; j < ne; j++) {
        const double x = edge[j] / 256.0;
        /* a pulse far shorter than a half-bit is noise: drop both its edges,
         * and the level carries on as it was */
        if (j + 1 < ne && (edge[j + 1] - edge[j]) / 256.0 < 0.4 * t) {
            j++;
            continue;
        }
        double k = floor((x - b) / t + 0.5);
        if (k < 1) k = 1;            /* early, but an edge all the same */
        for (int m = 0; m < (int)k; m++) cell(c, level);
        /* follow the edges: snap most of the way, and the period a little */
        const double expect = b + k * t;
        t += 0.05 * (x - expect) / k;
        b = expect + 0.7 * (x - expect);
        level = !level;
    }
    cell(c, level);                  /* the half after the last edge */
}

bool rfrf_decode(const uint32_t *env, rfrf_span_t span, const rfrf_cfg_t *cfg,
                 uint32_t *work, uint8_t *bits, size_t bits_cap, rfrf_burst_t *out) {
    rfrf_burst_t r;
    memset(&r, 0, sizeof r);
    r.span = span;
    const size_t n = span.end > span.start ? span.end - span.start : 0;
    if (out) *out = r;
    if (n < 16) return false;

    /* the two AM levels: two-means, from the 10th and 90th percentiles */
    double lo = percentile(env + span.start, n, work, 10);
    double hi = percentile(env + span.start, n, work, 90);
    for (int it = 0; it < 8 && hi > lo; it++) {
        const double s = (lo + hi) / 2;
        double sl = 0, sh = 0;
        size_t nl = 0, nh = 0;
        for (size_t k = span.start; k < span.end; k++) {
            if (env[k] < s) { sl += env[k]; nl++; }
            else { sh += env[k]; nh++; }
        }
        if (!nl || !nh) break;
        lo = sl / nl;
        hi = sh / nh;
    }
    r.slice = (uint32_t)((lo + hi) / 2);
    /* The levels themselves: the middle of each cluster, away from the
     * edges between them (the means are pulled toward each other by them) */
    {
        size_t nl = 0, nh = 0;
        for (size_t k = span.start; k < span.end; k++)
            if (env[k] < r.slice) work[nl++] = env[k];
        if (nl) lo = select_kth(work, nl, nl / 4);
        for (size_t k = span.start; k < span.end; k++)
            if (env[k] >= r.slice) work[nh++] = env[k];
        if (nh) hi = select_kth(work, nh, nh * 3 / 4);
    }
    r.lo = (uint32_t)lo;
    r.hi = (uint32_t)hi;
    r.depth = hi > 0 ? 1.0 - sqrt(lo / hi) : 0;
    if (out) *out = r;
    if (hi <= lo * 1.2) return false;          /* no modulation to slice */

    /* The burst's own rise and fall are not data edges: a span starts where
     * the envelope first crosses the noise threshold, on the way up to the
     * carrier. Look for edges only from where it first reaches the high
     * level to where it last leaves it -- the lead and tail are carrier. */
    /* Each end is judged against the carrier near it -- the highest envelope
     * within four slow half-bits -- not the burst's slice: a carrier that
     * fades would otherwise lose its tail. */
    rfrf_span_t data = span;
    {
        const size_t w = (size_t)ceil(4 * cfg->fs / (2 * cfg->rate_min));
        uint32_t head = 0, tail = 0;
        for (size_t k = span.start; k < span.end && k < span.start + w; k++)
            if (env[k] > head) head = env[k];
        for (size_t k = span.end > span.start + w ? span.end - w : span.start; k < span.end; k++)
            if (env[k] > tail) tail = env[k];
        while (data.start < data.end && env[data.start] < head / 10u * 7u) data.start++;
        while (data.end > data.start && env[data.end - 1] < tail / 10u * 7u) data.end--;
    }
    if (data.end - data.start < 16) return false;
    bool level0;
    /* the local slice looks a bit and a half (three half-bits at the
     * slowest rate) either side */
    const size_t window = (size_t)ceil(3 * cfg->fs / (2 * cfg->rate_min));
    const size_t ne = find_edges(env, data, r.slice, r.hi - r.lo, window, work, n, &level0);
    if (ne < 4) return false;
    /* positions from here on are from data.start; keep the lead from span's */
    const size_t lead_samples = data.start - span.start;

    /* the half-bit: from the hint, else from each end of the allowed rates */
    const double tmin = cfg->fs / (2 * cfg->rate_max), tmax = cfg->fs / (2 * cfg->rate_min);
    const double starts[3] = { cfg->fs / (2 * cfg->rate_hint), tmin, tmax };
    double best_t = 0;
    size_t best_in = 0;
    for (int s = 0; s < 3; s++) {
        size_t in;
        const double t = fit_half_bit(work, ne, starts[s], &in);
        if (t >= tmin * 0.97 && t <= tmax * 1.03 && in > best_in) { best_t = t; best_in = in; }
    }
    if (best_t == 0 || best_in < (ne - 1) / 2) return false;
    r.half_bit = best_t;
    r.rate = cfg->fs / (2 * best_t);
    r.lead = (size_t)floor((work[0] / 256.0 + (double)lead_samples) / best_t + 0.5);

    cells_t c;
    memset(&c, 0, sizeof c);
    c.prev[0] = c.prev[1] = -1;
    walk(work, ne, best_t, level0, &c);
    r.pairing = c.viol[1] < c.viol[0] ? 1 : 0;

    cells_t d;
    memset(&d, 0, sizeof d);
    d.prev[0] = d.prev[1] = -1;
    d.bits = bits;
    d.cap = bits_cap;
    d.pairing = r.pairing;
    d.pol = cfg->polarity;
    if (bits && bits_cap) memset(bits, 0, bits_cap);
    walk(work, ne, best_t, level0, &d);
    r.nbits = d.nbits;
    r.violations = d.viol[r.pairing];
    r.ok = r.nbits > 0 && r.violations * 8 <= r.nbits;
    if (out) *out = r;
    return r.ok;
}

/* ---- transmit --------------------------------------------------------------------- */

size_t rfrf_modulate(const uint8_t *bits, size_t nbits, const rfrf_tx_t *tx, float *amp, size_t cap) {
    const double th = tx->fs / (2 * tx->rate);                 /* samples per half-bit */
    const size_t halves = 2u * (tx->lead_bits + nbits + tx->tail_bits);
    const double tau = tx->rise > 0 ? tx->rise / 2.197 : 0;    /* 10-90% of one pole */
    const double alpha = tau > 0 ? 1.0 - exp(-1.0 / (tx->fs * tau)) : 1.0;
    const size_t settle = tau > 0 ? (size_t)ceil(6 * tau * tx->fs) : 0;
    const size_t total = (size_t)ceil(halves * th) + settle;
    if (!amp) return total;

    const double low = 1.0 - tx->depth;
    double y = 0;
    for (size_t k = 0; k < total && k < cap; k++) {
        const size_t h = (size_t)floor((k + 0.5) / th);
        double target = 0;
        if (h < halves) {
            const size_t bit = h / 2;
            if (bit < tx->lead_bits || bit >= tx->lead_bits + nbits) {
                target = 1.0;                                   /* lead and tail: carrier */
            } else {
                const size_t d = bit - tx->lead_bits;
                int v = bits[d / 8] >> (7 - d % 8) & 1;
                if (tx->polarity == RFRF_THOMAS) v = !v;
                /* IEEE: 0 high-low, 1 low-high */
                const bool first = (h & 1u) == 0;
                const bool high = v ? !first : first;
                target = high ? 1.0 : low;
            }
        }
        y += alpha * (target - y);
        amp[k] = (float)y;
    }
    return total;
}
