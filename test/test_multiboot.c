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
        /* second 0x63D1: hand over the chosen client byte */
        g->state = SIM_KEY3;
        uint32_t r = ((uint32_t)g->client << 16) | 0x000063D1u;
        g->m = ((r & 0x00FF0000u) >> 8) + 0xFFFF00D1u;
        return r;
    }

    case SIM_KEY3:
        /* master sends 0x64xx; we reply with a byte that seeds f */
        g->state = SIM_LEN;
        return ((uint32_t)g->client << 16) | 0x00006400u;

    case SIM_LEN: {
        g->payload_words = w;
        uint32_t h = (uint32_t)g->client + 0x0F;
        g->f = ((((uint32_t)g->client << 16) & 0x00FF0000u) >> 8);
        g->f = (g->f + h) | 0xFFFF0000u;
        g->state = SIM_DATA;
        g->offset = MB_HEADER;
        return ((uint32_t)g->client << 16) | 0x00000000u;
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

    /* And the payload must decrypt to exactly the original bytes. This is the
     * assertion that actually validates the crypto: a wrong multiplier, xor or
     * address term all show up right here. */
    check("payload decrypts byte-identically",
          memcmp(gba.ewram + MB_HEADER, g_rom + MB_HEADER, size - MB_HEADER), 0);

    /* Both sides computed the same checksum independently. */
    check("checksums agree", s.crc_expected, s.crc_received);
    check("slave agrees too", (long)(gba.crc & 0xFFFF), s.crc_expected);
    printf("  crc 0x%04X\n", s.crc_expected);
}

static void test_padding(void) {
    printf("unaligned ROM\n");

    /* 0x1A5 is not a multiple of 16, so the tail must be zero padded rather
     * than reading past the caller's buffer. */
    const size_t size = 0x1A5;
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

    /* Argument checking. */
    sim_gba_t ok;
    sim_init(&ok, 0xD1);
    mb_init(&s, sim_xfer_top, &ok);
    check("null rom", mb_send(&s, NULL, 0x400), MB_ERR_ARGS);
    check("too small", mb_send(&s, g_rom, 0x40), MB_ERR_ARGS);
    check("too big", mb_send(&s, g_rom, MB_MAX_SIZE + 4), MB_ERR_ARGS);
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
    test_failures();
    test_progress();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
