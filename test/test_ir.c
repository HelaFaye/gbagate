/*
 * test_ir.c -- the Cyber Drive Zoids infrared protocol.
 *
 *   cc -std=c11 -Wall -Wextra -o test_ir test_ir.c ir_cdz.c
 *
 * The protocol is Shonumi's reverse engineering; these tests pin down the
 * reading of it, which is where an implementation goes wrong. Two mistakes in
 * particular are easy and silent: classifying by ON duration instead of the
 * gap, and counting the keep-alive Minis as bits.
 */

#include <stdio.h>
#include <string.h>

#include "ir_cdz.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-46s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* Replay a list of gaps as edge timestamps, as the hardware would deliver. */
static int replay(cdz_rx_t *rx, const uint32_t *gaps, size_t n, uint16_t *out) {
    uint32_t t = 1000;
    int got = 0;
    cdz_rx_edge(rx, t, NULL);          /* first edge only starts the clock */
    for (size_t i = 0; i < n; i++) {
        t += gaps[i];
        uint16_t cmd;
        if (cdz_rx_edge(rx, t, &cmd)) {
            if (out) out[got] = cmd;
            got++;
        }
    }
    return got;
}

static void test_classification(void) {
    printf("pulse classes\n");

    check("nominal mini",   cdz_classify(CDZ_MINI_US),   CDZ_PULSE_MINI);
    check("nominal short",  cdz_classify(CDZ_SHORT_US),  CDZ_PULSE_SHORT);
    check("nominal medium", cdz_classify(CDZ_MEDIUM_US), CDZ_PULSE_MEDIUM);
    check("nominal long",   cdz_classify(CDZ_LONG_US),   CDZ_PULSE_LONG);

    /* The article gives mini as a range, not a value. */
    check("mini low end",  cdz_classify(18), CDZ_PULSE_MINI);
    check("mini high end", cdz_classify(20), CDZ_PULSE_MINI);

    /*
     * Short and Medium are only 2.5x apart, and Medium is the one a late
     * interrupt stretches. Both need real margin either side.
     */
    check("short stretched 40%", cdz_classify(CDZ_SHORT_US * 14 / 10), CDZ_PULSE_SHORT);
    check("medium early 30%",    cdz_classify(CDZ_MEDIUM_US * 7 / 10), CDZ_PULSE_MEDIUM);
    check("medium stretched 3x", cdz_classify(CDZ_MEDIUM_US * 3), CDZ_PULSE_MEDIUM);

    /* A Long is 150x a Medium, so even a wildly late one is unambiguous. */
    check("long arriving early", cdz_classify(CDZ_LONG_US / 4), CDZ_PULSE_LONG);
    check("zero gap is a mini", cdz_classify(0), CDZ_PULSE_MINI);
}

static void test_round_trip(void) {
    printf("encode then decode\n");

    uint32_t gaps[CDZ_CMD_BITS + 1];
    uint16_t cmds[4];

    static const uint16_t samples[] = {
        0x000, 0xFFF, 0xAAA, 0x555, 0x123, 0x8F1, 0x001, 0x800
    };
    for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        size_t n = cdz_encode(samples[i], gaps, sizeof gaps / sizeof gaps[0]);
        check("encoded length", (long)n, CDZ_CMD_BITS + 1);

        cdz_rx_t rx;
        cdz_rx_init(&rx);
        int got = replay(&rx, gaps, n, cmds);
        if (got != 1 || cmds[0] != samples[i]) {
            printf("  FAIL %03X round trip: got %d commands, first %03X\n",
                   samples[i], got, got ? cmds[0] : 0);
            g_fail++;
        }
    }
    check("small buffer refused", (long)cdz_encode(0x123, gaps, 4), 0);

    /* MSB first, so the top field is the first four bits on the wire. */
    cdz_encode(0x800, gaps, sizeof gaps / sizeof gaps[0]);
    check("first bit of 0x800 is a medium",
          cdz_classify(gaps[0]), CDZ_PULSE_MEDIUM);
    check("second is a short", cdz_classify(gaps[1]), CDZ_PULSE_SHORT);
}

static void test_minis_are_not_bits(void) {
    printf("keep-alives carry no data\n");

    /*
     * The article says Minis are "thrown in between every other type of
     * pulse". Counting them as bits gives a decoder that overruns twelve and
     * discards every command -- which looks like a timing problem, not a
     * parsing one.
     */
    uint32_t plain[CDZ_CMD_BITS + 1];
    cdz_encode(0x5A3, plain, sizeof plain / sizeof plain[0]);

    uint32_t peppered[64];
    size_t n = 0;
    for (size_t i = 0; i < CDZ_CMD_BITS + 1u; i++) {
        peppered[n++] = CDZ_MINI_US;      /* before */
        peppered[n++] = plain[i];
        peppered[n++] = CDZ_MINI_US;      /* and after */
    }

    cdz_rx_t rx;
    cdz_rx_init(&rx);
    uint16_t cmds[4];
    int got = replay(&rx, peppered, n, cmds);
    check("still one command", got, 1);
    check("and the right one", got ? cmds[0] : 0, 0x5A3);
    check("minis counted separately", rx.minis > 0, 1);
    check("nothing aborted", rx.aborted, 0);
}

static void test_malformed(void) {
    printf("malformed trains\n");

    cdz_rx_t rx;
    cdz_rx_init(&rx);

    /* Too few bits before the terminator: discarded, not padded. */
    uint32_t shortish[6];
    for (int i = 0; i < 5; i++) shortish[i] = CDZ_SHORT_US;
    shortish[5] = CDZ_LONG_US;
    check("short train rejected", replay(&rx, shortish, 6, NULL), 0);
    check("counted as aborted", rx.aborted, 1);

    /* Too many: also discarded, and the next command must still decode --
     * a decoder that keeps a partial train merges it with the next one. */
    uint32_t longish[20];
    for (int i = 0; i < 19; i++) longish[i] = CDZ_MEDIUM_US;
    longish[19] = CDZ_LONG_US;
    cdz_rx_init(&rx);
    check("long train rejected", replay(&rx, longish, 20, NULL), 0);

    uint32_t good[CDZ_CMD_BITS + 1];
    cdz_encode(0x321, good, sizeof good / sizeof good[0]);
    uint16_t cmd = 0;
    int got = replay(&rx, good, CDZ_CMD_BITS + 1, &cmd);
    check("recovers on the next command", got, 1);
    check("and decodes it correctly", cmd, 0x321);
}

static void test_clock_wrap(void) {
    printf("a wrapped microsecond counter\n");

    /* esp_timer_get_time() truncated to 32 bits wraps every ~72 minutes, and
     * a session can straddle that. Unsigned subtraction has to carry it. */
    cdz_rx_t rx;
    cdz_rx_init(&rx);

    uint32_t t = 0xFFFFFF00u;
    cdz_rx_edge(&rx, t, NULL);

    uint32_t gaps[CDZ_CMD_BITS + 1];
    cdz_encode(0xC5A, gaps, sizeof gaps / sizeof gaps[0]);

    uint16_t cmd = 0;
    int got = 0;
    for (size_t i = 0; i < CDZ_CMD_BITS + 1u; i++) {
        t += gaps[i];                     /* wraps partway through */
        if (cdz_rx_edge(&rx, t, &cmd)) got++;
    }
    check("decoded across the wrap", got, 1);
    check("value intact", cmd, 0xC5A);
}

static void test_fields(void) {
    printf("command fields\n");

    /* Three 4-bit fields, MSB first: category, motion, action. */
    check("category", cdz_category(0x123), 0x1);
    check("motion",   cdz_motion(0x123),   0x2);
    check("action",   cdz_action(0x123),   0x3);
    check("top nibble ignored above 12 bits", cdz_category(0xF123), 0x1);
}

int main(void) {
    test_classification();
    test_round_trip();
    test_minis_are_not_bits();
    test_malformed();
    test_clock_wrap();
    test_fields();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
