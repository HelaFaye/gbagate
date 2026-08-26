/*
 * test_multiboot.c -- multiboot protocol against a simulated GBA BIOS.
 *
 *   cc -std=c11 -Wall -Wextra -o test_multiboot test_multiboot.c gba_multiboot.c
 *
 * The simulated slave is not a stub that says yes: it runs the receiving half
 * of the protocol, derives the same session key from the client byte it chose,
 * decrypts every word into an EWRAM image and computes its own CRC. A test
 * passes only if the bytes that land in simulated EWRAM are byte-identical to
 * the ROM and both sides independently arrive at the same checksum.
 *
 * That is the property worth testing, because the failure mode on real
 * hardware is a transfer that completes and then fails the CRC with no
 * indication of which constant was wrong.
 */

#include <stdio.h>
#include <string.h>

#include "gba_multiboot.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-42s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* ---- simulated GBA ----------------------------------------------------- */

typedef enum {
    SIM_SYNC = 0, SIM_RECOG, SIM_HEADER, SIM_HDR_DONE, SIM_SYNC2,
    SIM_KEY1, SIM_KEY2, SIM_KEY3, SIM_LEN, SIM_DATA, SIM_CRC_POLL,
    SIM_CRC_REQ, SIM_CRC_VAL, SIM_DONE
} sim_state_t;

typedef struct {
    sim_state_t state;
    uint8_t     client;      /* the byte this GBA picks */
    uint32_t    m;           /* its copy of the session key */
    uint32_t    crc;
    uint32_t    offset;      /* bytes accepted */
    uint32_t    header_words;
    uint32_t    payload_words;
    uint32_t    f;
    uint8_t     ewram[MB_MAX_SIZE];
    uint8_t     header[MB_HEADER];
    uint32_t    crc_from_master;

    /* knobs for the failure paths */
    uint32_t sync_delay;     /* words to ignore before answering the sync */
    bool     corrupt_data;   /* flip a bit in one received word */
    bool     wrong_echo;     /* echo a bogus offset back                  */
    bool     mute;           /* never answer at all                       */
} sim_gba_t;

static void sim_init(sim_gba_t *g, uint8_t client) {
    memset(g, 0, sizeof *g);
    g->client = client;
    g->crc    = MB_CRC_SEED;
}

static uint32_t sim_xfer(void *ctx, uint32_t w) {
    sim_gba_t *g = ctx;

    if (g->mute) {
        return 0x00000000u;
    }

    switch (g->state) {
    case SIM_SYNC:
        if (g->sync_delay) {
            g->sync_delay--;
            return 0x00000000u;
        }
        if (w == 0x00006202u) {
            g->state = SIM_RECOG;
            return 0x72026202u;
        }
        return 0x00000000u;

    case SIM_RECOG:
        /* master repeats the sync, then sends recognition */
        if (w == 0x00006102u) {
            g->state = SIM_HEADER;
        }
        return 0x72026202u;

    case SIM_HEADER:
        g->header[g->header_words * 2]     = (uint8_t)(w & 0xFF);
        g->header[g->header_words * 2 + 1] = (uint8_t)((w >> 8) & 0xFF);
        if (++g->header_words == MB_HEADER / 2) {
            g->state = SIM_HDR_DONE;
        }
        return 0x00000000u;

    case SIM_HDR_DONE:
        g->state = SIM_SYNC2;
        return 0x00000000u;

    case SIM_SYNC2:
        g->state = SIM_KEY1;
        return 0x00000000u;

    case SIM_KEY1:
        /* first 0x63D1 primes; answer without the client byte */
        g->state = SIM_KEY2;
        return 0x00000000u;

    case SIM_KEY2: {
        /*
         * Second 0x63D1: hand over the chosen client byte, and build our copy
         * of the session key.
         *
         * GBATEK: m = dword(pp,cc,cc,cc), packed least-significant byte first,
         * where pp is the palette byte the master just sent us as 63pp and the
         * two absent slaves on a Normal-mode link read back as FFh.
         *
         * This is derived from the spec rather than copied from the master's
         * expression on purpose: a slave that mirrors the master's arithmetic
         * agrees with it even when the arithmetic is wrong, which makes the
         * whole round-trip test vacuous for exactly the bugs it should catch.
         */
        g->state = SIM_KEY3;
        uint32_t pp = w & 0xFFu;
        g->m = 0xFFFF0000u | ((uint32_t)g->client << 8) | pp;
        /* Normal mode: high half is the slave's word, low half echoes ours. */
        return ((uint32_t)g->client << 16) | (w & 0xFFFFu);
    }

    case SIM_KEY3:
        /* master sends 0x64xx; we reply with a byte that seeds f */
        g->state = SIM_LEN;
        return ((uint32_t)g->client << 16) | 0x00006400u;

    case SIM_LEN: {
        g->payload_words = w;
        /*
         * GBATEK: f = dword(hh,rr,rr,rr), packed least-significant byte first,
         * where hh = 11h + client_data[1] + client_data[2] + client_data[3]
         * and the two absent slaves contribute FFh apiece. hh is a byte-wide
         * field -- it goes out on the wire as the low half of 64hh -- so it is
         * masked before packing. rr is the random byte we return below.
         *
         * Spelled out in GBATEK's terms rather than reduced, so the mask is
         * visibly a consequence of the spec and not of the master's code.
         */
        uint32_t hh = ((uint32_t)g->client + 0x11u + 0xFFu + 0xFFu) & 0xFFu;
        uint32_t rr = g->client;
        g->f = 0xFFFF0000u | (rr << 8) | hh;
        g->state = SIM_DATA;
        g->offset = MB_HEADER;
        return rr << 16;
    }

    case SIM_DATA: {
        g->m = mb_key_next(g->m);
        /* The master masks by bytes-accepted, which starts at 0xC0. */
        uint32_t plain = w ^ mb_word_mask(g->m, MB_LOAD_ADDR + g->offset);

        if (g->corrupt_data && g->offset == MB_HEADER + 8) {
            plain ^= 1u;
        }

        g->ewram[g->offset]     = (uint8_t)(plain & 0xFF);
        g->ewram[g->offset + 1] = (uint8_t)((plain >> 8) & 0xFF);
        g->ewram[g->offset + 2] = (uint8_t)((plain >> 16) & 0xFF);
        g->ewram[g->offset + 3] = (uint8_t)((plain >> 24) & 0xFF);

        g->crc = mb_crc_step(g->crc, plain);

        uint32_t echo = ((g->offset & 0xFFFFu) << 16) | 0x0000u;
        if (g->wrong_echo && g->offset == MB_HEADER + 16) {
            echo = 0xDEAD0000u;
        }
        g->offset += 4;
        return echo;
    }

    case SIM_CRC_POLL:
        return 0x00750065u;

    default:
        return 0x00000000u;
    }
}

/*
 * The master leaves SIM_DATA by polling 0x65, so intercept that here rather
 * than trying to count words: the payload length maths differs between the two
 * sides and counting would just re-implement the bug we are testing for.
 */
static uint32_t sim_xfer_top(void *ctx, uint32_t w) {
    sim_gba_t *g = ctx;

    if (g->state == SIM_DATA && w == 0x00000065u) {
        g->crc = mb_crc_step(g->crc, g->f);
        g->state = SIM_CRC_REQ;
        return 0x00750065u;
    }
    if (g->state == SIM_CRC_REQ) {
        if (w == 0x00000066u) {
            g->state = SIM_CRC_VAL;
            return 0x00000000u;
        }
        return 0x00750065u;
    }
    if (g->state == SIM_CRC_VAL) {
        g->crc_from_master = w;
        g->state = SIM_DONE;
        return ((g->crc & 0xFFFFu) << 16) | 0x66u;
    }
    return sim_xfer(ctx, w);
}

/* ---- a plausible ROM --------------------------------------------------- */

static uint8_t g_rom[0x2000];

static void make_rom(size_t size) {
    memset(g_rom, 0, sizeof g_rom);
    /* Header: entry branch, then filler standing in for the logo. */
    g_rom[0] = 0x2E; g_rom[1] = 0x00; g_rom[2] = 0x00; g_rom[3] = 0xEA;
    for (size_t i = 4; i < MB_HEADER; i++) {
        g_rom[i] = (uint8_t)(0x51 + i * 7);
    }
    for (size_t i = MB_HEADER; i < size; i++) {
        g_rom[i] = (uint8_t)((i * 31u) ^ (i >> 5));
    }
}

/* ---- tests -------------------------------------------------------------- */

static void test_primitives(void) {
    printf("primitives\n");

    /* The constants are quoted from a working implementation; assert the exact
     * values so a typo cannot survive a refactor. */
    check("key multiplier", MB_KEY_MULT, 0x6F646573);
    check("key xor", MB_KEY_XOR, 0x43202F2F);
    check("crc polynomial", MB_CRC_POLY, 0xC37B);
    check("crc seed", MB_CRC_SEED, 0xC387);

    check("key steps", mb_key_next(0), 1);
    check("key steps again", mb_key_next(1), (long)(uint32_t)(MB_KEY_MULT + 1));

    /* The mask depends on the destination address, so the same word encrypts
     * differently at different offsets. That is what stops a replayed block
     * from landing anywhere useful. */
    uint32_t m = mb_key_next(12345);
    check("mask varies with address",
          mb_word_mask(m, MB_LOAD_ADDR) != mb_word_mask(m, MB_LOAD_ADDR + 4), 1);

    /* Round trip. */
    uint32_t plain = 0xDEADBEEF;
    uint32_t enc = plain ^ mb_word_mask(m, MB_LOAD_ADDR + 0x40);
    check("mask is its own inverse",
          (long)(uint32_t)(enc ^ mb_word_mask(m, MB_LOAD_ADDR + 0x40)), (long)plain);

    check("crc changes with input", mb_crc_step(MB_CRC_SEED, 0) != mb_crc_step(MB_CRC_SEED, 1), 1);

    check("padding rounds to 16", (long)mb_padded_size(MB_HEADER + 1), MB_HEADER + 16);
    check("padding leaves aligned alone", (long)mb_padded_size(MB_HEADER + 32), MB_HEADER + 32);
}

static void test_full_transfer(void) {
    printf("full transfer\n");

    const size_t size = 0x800;
    make_rom(size);

    sim_gba_t gba;
    sim_init(&gba, 0xD1);

    mb_session_t s;
    mb_init(&s, sim_xfer_top, &gba);

    mb_result_t r = mb_send(&s, g_rom, size);
    printf("  result: %s, %u words, client byte 0x%02X\n",
           mb_result_name(r), s.words_sent, s.client_data);
    check("transfer succeeds", r, MB_OK);

    /* The header must have arrived verbatim. */
    check("header intact", memcmp(gba.header, g_rom, MB_HEADER), 0);

    /*
     * And the payload must decrypt to exactly the original bytes.
     *
     * Be precise about what that proves. The slave calls the same
     * mb_key_next/mb_word_mask/mb_crc_step as the master, so a wrong value for
     * MB_KEY_MULT or MB_KEY_XOR cancels out and still passes here -- only the
     * literal pins in test_primitives() catch those. What this does catch is
     * the two sides *disagreeing*: a mis-stepped key, a mis-tracked offset, or
     * a handshake word packed differently at each end.
     */
    check("payload decrypts byte-identically",
          memcmp(gba.ewram + MB_HEADER, g_rom + MB_HEADER, size - MB_HEADER), 0);

    /* Both sides computed the same checksum independently. */
    check("checksums agree", s.crc_expected, s.crc_received);
    check("slave agrees too", (long)(gba.crc & 0xFFFF), s.crc_expected);
    printf("  crc 0x%04X\n", s.crc_expected);
}

static void test_padding(void) {
    printf("unaligned ROM\n");

    /* 0x1C5 is not a multiple of 16, so the tail must be zero padded rather
     * than reading past the caller's buffer. (It is also the smallest such
     * size at or above MB_MIN_SIZE.) */
    const size_t size = 0x1C5;
    make_rom(size);

    sim_gba_t gba;
    sim_init(&gba, 0x42);
    mb_session_t s;
    mb_init(&s, sim_xfer_top, &gba);

    check("succeeds", mb_send(&s, g_rom, size), MB_OK);
    check("real bytes intact",
          memcmp(gba.ewram + MB_HEADER, g_rom + MB_HEADER, size - MB_HEADER), 0);

    size_t padded = mb_padded_size(size);
    int nonzero = 0;
    for (size_t i = size; i < padded; i++) {
        if (gba.ewram[i]) nonzero++;
    }
    check("padding is zeros", nonzero, 0);
    check("padded to 16", (long)(padded % 16), 0);
}

/*
 * The client byte is chosen at random by the GBA, so any bug whose trigger is
 * a property of that byte shows up as an intermittent failure on hardware and
 * as a pass in any test that happens to pick a friendly value.
 *
 * Both the session key and the final checksum word are assembled from it as
 * packed bytes. hh = 11h+cc+FFh+FFh overflows a byte once cc > F0h, so a master
 * that assembles f by addition rather than by packing corrupts the neighbouring
 * rr byte for 14 of the 254 legal client values -- about 5% of real transfers,
 * each failing at the very last word with MB_ERR_CRC and nothing to point at.
 *
 * Sweeping the whole legal range costs a few milliseconds and removes the luck.
 */
static void test_client_byte_range(void) {
    printf("client byte range\n");

    const size_t size = 0x400;
    make_rom(size);

    int bad_result = 0, bad_payload = 0, bad_crc = 0;
    unsigned first_bad = 0;

    for (unsigned cc = 0x01; cc <= 0xFEu; cc++) {
        sim_gba_t gba;
        sim_init(&gba, (uint8_t)cc);
        mb_session_t s;
        mb_init(&s, sim_xfer_top, &gba);

        int ok = 1;
        if (mb_send(&s, g_rom, size) != MB_OK) { bad_result++;  ok = 0; }
        if (memcmp(gba.ewram + MB_HEADER, g_rom + MB_HEADER, size - MB_HEADER)) {
            bad_payload++; ok = 0;
        }
        if (s.crc_expected != s.crc_received) { bad_crc++; ok = 0; }

        if (!ok && !first_bad) {
            first_bad = cc;
            printf("  first failing client byte: 0x%02X "
                   "(master crc 0x%04X, slave crc 0x%04X)\n",
                   cc, s.crc_expected, s.crc_received);
        }
    }

    check("every client byte transfers", bad_result, 0);
    check("every client byte decrypts", bad_payload, 0);
    check("every client byte agrees on the checksum", bad_crc, 0);
}

static void test_failures(void) {
    printf("failure paths\n");

    make_rom(0x400);
    mb_session_t s;

    /* No console attached. */
    sim_gba_t mute;
    sim_init(&mute, 0xD1);
    mute.mute = true;
    mb_init(&s, sim_xfer_top, &mute);
    s.sync_attempts = 50;
    check("no GBA reported", mb_send(&s, g_rom, 0x400), MB_ERR_NO_GBA);
    check("gave up after the limit", s.sync_tries, 50);

    /* Console switched on late: the sync must be patient, not one-shot. */
    sim_gba_t slow;
    sim_init(&slow, 0xD1);
    slow.sync_delay = 30;
    mb_init(&s, sim_xfer_top, &slow);
    check("late console still works", mb_send(&s, g_rom, 0x400), MB_OK);
    check("needed several tries", s.sync_tries > 1, 1);

    /* A corrupted word must surface as a checksum failure, not silent success. */
    sim_gba_t bad;
    sim_init(&bad, 0xD1);
    bad.corrupt_data = true;
    mb_init(&s, sim_xfer_top, &bad);
    check("corruption caught", mb_send(&s, g_rom, 0x400), MB_ERR_CRC);
    check("checksums differ", s.crc_expected != s.crc_received, 1);

    /* A bogus offset echo must abort immediately rather than sending the whole
     * ROM and failing at the end. */
    sim_gba_t drifted;
    sim_init(&drifted, 0xD1);
    drifted.wrong_echo = true;
    mb_init(&s, sim_xfer_top, &drifted);
    check("stall detected", mb_send(&s, g_rom, 0x400), MB_ERR_STALLED);
    check("aborted early", s.words_sent < 200, 1);

    /* Argument checking. The bounds are GBATEK's, quoted in the header: a
     * payload -- excluding the 0xC0 header -- of 0x100 to 0x3FF40 bytes. */
    sim_gba_t ok;
    sim_init(&ok, 0xD1);
    mb_init(&s, sim_xfer_top, &ok);
    check("null rom", mb_send(&s, NULL, 0x400), MB_ERR_ARGS);
    check("too small", mb_send(&s, g_rom, 0x40), MB_ERR_ARGS);
    check("one byte under the floor",
          mb_send(&s, g_rom, MB_MIN_SIZE - 1), MB_ERR_ARGS);
    check("too big", mb_send(&s, g_rom, MB_MAX_SIZE + 4), MB_ERR_ARGS);
    /* Pinned as literals, not as MB_HEADER+0x100: comparing a macro against
     * its own definition asserts nothing. These are the numbers GBATEK's
     * payload range works out to for a whole image. */
    check("floor is GBATEK's 0x100 payload", MB_MIN_SIZE, 0x1C0);
    check("ceiling is GBATEK's 0x3FF40 payload", MB_MAX_SIZE, 0x40000);

    /* And the floor itself must still transfer, not just be accepted. */
    sim_gba_t smallest;
    sim_init(&smallest, 0xD1);
    mb_init(&s, sim_xfer_top, &smallest);
    make_rom(MB_MIN_SIZE);
    check("the floor itself transfers",
          mb_send(&s, g_rom, MB_MIN_SIZE), MB_OK);
}

static void test_progress(void) {
    printf("progress reporting\n");

    make_rom(0x2000);
    sim_gba_t gba;
    sim_init(&gba, 0x77);
    mb_session_t s;
    mb_init(&s, sim_xfer_top, &gba);

    s.progress = NULL; /* must be safe to leave unset */
    check("no callback is fine", mb_send(&s, g_rom, 0x2000), MB_OK);
}

int main(void) {
    test_primitives();
    test_full_transfer();
    test_padding();
    test_client_byte_range();
    test_failures();
    test_progress();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
