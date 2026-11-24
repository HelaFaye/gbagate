/* test_codec.c -- the RMT link test's waveforms, on the host.
 *   cc -std=c11 -Wall -Wextra -I../main -o test_codec test_codec.c ../main/rl_codec.c && ./test_codec */
#include <stdio.h>
#include <stdlib.h>
#include "rl_codec.h"

static int fails, checks;
#define CHECK(what, ok) do { checks++; if (!(ok)) { fails++; printf("  FAIL  %s\n", what); } } while (0)

/* A TX symbol list as the line shows it: idle high before, idle high after. */
static int line(const uint32_t *sym, int n, rl_run_t *out, int max) {
    int m = 0;
    out[m++] = (rl_run_t){1, 1000000};
    m += rl_sym_to_runs(sym, n, out + m, max - m);
    if (out[m - 1].ticks == 0) m--;            /* the end marker */
    /* join a trailing high to the idle that follows */
    if (out[m - 1].level == 1) out[m - 1].ticks += 1000000;
    else out[m++] = (rl_run_t){1, 1000000};
    return m;
}

static int mp_round(uint16_t w, uint32_t max_ns, uint32_t bit_ns, uint16_t *got) {
    uint32_t sym[32];
    rl_run_t ln[64], rx[64];
    const int n = rl_mp_encode(w, bit_ns, sym, 32);
    if (n < 0) return -100;
    const int m = line(sym, n, ln, 64);
    const int r = rl_rx_model(ln, m, max_ns, rx, 64);
    return rl_mp_decode(rx, r, bit_ns, got);
}

int main(void) {
    printf("Multi-Player frames\n");
    uint16_t words[1100] = {0x0000, 0xFFFF, 0x5555, 0xAAAA, 0x1234, 0x8001, 0x7FFE, 0x00FF, 0xFF00, 0x0001, 0x8000};
    int nw = 11;
    srand(7);
    while (nw < 1100) words[nw++] = (uint16_t)rand();
    int ok200 = 0, predicted = 0, mismatch = 0;
    for (int k = 0; k < nw; k++) {
        uint16_t got = 0;
        ok200 += mp_round(words[k], 200000, RL_MP_NS, &got) == RL_OK && got == words[k];
        got = 0;
        const int r60 = mp_round(words[k], 60000, RL_MP_NS, &got);
        const int should_fail = rl_mp_longest_run(words[k]) * RL_MP_NS > 60000;
        predicted += should_fail;
        mismatch += should_fail != !(r60 == RL_OK && got == words[k]);
    }
    char msg[160];
    snprintf(msg, sizeof msg, "idle threshold 200 us: every word comes back (%d/%d)", ok200, nw);
    CHECK(msg, ok200 == nw);
    snprintf(msg, sizeof msg, "idle threshold 60 us: exactly the words with a run over 60 us fail (%d of %d; %d differ)",
             predicted, nw, mismatch);
    CHECK(msg, mismatch == 0 && predicted > 0);
    uint16_t got;
    CHECK("0x0000 (17 bits low, 147.6 us) is cut short at 60 us", mp_round(0x0000, 60000, RL_MP_NS, &got) == RL_ERR_SHORT);
    CHECK("0xFFFF passes at 60 us: its long run is the stop bit into idle", mp_round(0xFFFF, 60000, RL_MP_NS, &got) == RL_OK && got == 0xFFFF);
    CHECK("0x5555 passes at 60 us", mp_round(0x5555, 60000, RL_MP_NS, &got) == RL_OK && got == 0x5555);
    /* the worse case: a long HIGH run mid-frame ends the capture too, and what
     * is left looks like the stop bit running into idle -- a wrong word, no error */
    CHECK("0xF87F at 60 us: no error, but read as 0xFFFF (a long high run looks like the stop bit)",
          mp_round(0xF87F, 60000, RL_MP_NS, &got) == RL_OK && got == 0xFFFF);

    /* edges at round(k * bit): the frame's length is exact to a tick */
    uint32_t sym[32];
    const int n = rl_mp_encode(0x5555, RL_MP_NS, sym, 32);
    rl_run_t r[64];
    const int m = rl_sym_to_runs(sym, n, r, 64);
    uint32_t total = 0;
    for (int k = 0; k < m; k++) total += r[k].ticks;
    snprintf(msg, sizeof msg, "a frame is 18 bits long to the tick (%u of %u)", total, rl_ticks(18ull * RL_MP_NS));
    CHECK(msg, total == rl_ticks(18ull * RL_MP_NS));
    CHECK("0x5555: 18 runs, in 10 symbols with the end marker", m == 19 && n == 10);
    rl_run_t bad[3] = {{0, rl_ticks(RL_MP_NS * 3 / 2)}, {1, 0}};
    CHECK("a run of one and a half bits is a timing error", rl_mp_decode(bad, 2, RL_MP_NS, &got) == RL_ERR_TIMING);
    /* 2% slow and fast bits still decode (a UART's tolerance) */
    int drift = 1;
    for (int k = 0; k < 64; k++) {
        drift &= mp_round(words[k], 200000, RL_MP_NS * 102 / 100, &got) == RL_OK && got == words[k];
        drift &= mp_round(words[k], 200000, RL_MP_NS * 98 / 100, &got) == RL_OK && got == words[k];
    }
    CHECK("a sender 2% fast or slow still decodes (the decoder's bit is nominal)", drift);

    printf("normal mode\n");
    const uint32_t nwords[] = {0x12345678, 0x7FFFFFFF, 0x00000000, 0x55AA55AA, 0x0F0F0F0F, 0x40000001};
    const uint32_t rates[] = {3906u, 500u};      /* 256 kHz, 2 MHz: ns a bit */
    for (int q = 0; q < 2; q++) {
        int all = 1;
        for (unsigned k = 0; k < sizeof nwords / sizeof nwords[0]; k++) {
            uint32_t sc[48], so[48];
            int nsc, nso;
            all &= rl_normal_encode(nwords[k], rates[q], sc, &nsc, so, &nso, 48) == 0 && nsc <= 48 && nso <= 48;
            rl_run_t lsc[80], lso[80], rsc[80], rso[80];
            const int a = line(sc, nsc, lsc, 80), b = line(so, nso, lso, 80);
            /* SO can hold one level for all 32 bits: 125 us at 256 kHz */
            const int ra = rl_rx_model(lsc, a, 200000, rsc, 80), rb = rl_rx_model(lso, b, 200000, rso, 80);
            uint32_t w = 0;
            all &= rl_normal_decode(rsc, ra, rso, rb, &w) == RL_OK && w == nwords[k];
        }
        snprintf(msg, sizeof msg, "%s: six words (bit 31 clear) through SC and SO, back (in 48 symbols each)",
                 q ? "2 MHz" : "256 kHz");
        CHECK(msg, all);
    }
    {
        /* and at 256 kHz a 60 us threshold cuts SO's long runs too */
        uint32_t sc[48], so[48];
        int nsc, nso;
        rl_normal_encode(0x0000FFFF, 3906u, sc, &nsc, so, &nso, 48);
        rl_run_t lsc[80], lso[80], rsc[80], rso[80];
        const int a = line(sc, nsc, lsc, 80), b = line(so, nso, lso, 80);
        const int ra = rl_rx_model(lsc, a, 60000, rsc, 80), rb = rl_rx_model(lso, b, 60000, rso, 80);
        uint32_t w = 1;
        const int r = rl_normal_decode(rsc, ra, rso, rb, &w);
        CHECK("256 kHz, 0x0000FFFF at a 60 us threshold: SO's 62.5 us low run ends the capture, the highs after it are lost", r != RL_OK || w != 0x0000FFFF);
    }
    printf("test_codec: %d checks, %s\n", checks, fails ? "FAILED" : "all passed");
    return fails != 0;
}
