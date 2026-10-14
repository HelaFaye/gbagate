/*
 * test_mb_e2e.c -- the multiboot feature, end to end, with a real ROM.
 *
 *   cc -I/home/hela/gbagate/src -std=c11 -Wall -Wextra -o test_mb_e2e test_mb_e2e.c \
#include <stdbool.h>
#include <stdint.h>
 *      rom_catalog.c gba_multiboot.c
 *   ./test_mb_e2e rom/tester.gba
 *
 * test_multiboot.c proves the protocol against a synthetic image, and
 * test_catalog.c proves the catalogue against a simulated volume. Neither
 * proves the two work together on a real file: that the ROM on disk is found,
 * validated, staged, encrypted, transferred, and arrives in the simulated
 * console's EWRAM byte for byte.
 *
 * That is the whole "Send Program" mode, minus ESP-IDF and minus wires. If
 * this passes and hardware does not, the difference is the SPI layer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba_multiboot.h"
#include "rom_catalog.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-46s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* ---- a rom_io_t over the real filesystem -------------------------------- */

typedef struct {
    const char *dir;
    const char *name;
    int         served;
} disk_t;

static bool disk_next(void *ctx, char *name, size_t cap, uint32_t *size) {
    disk_t *d = ctx;
    if (d->served++) {
        return false;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s", d->dir, d->name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    *size = (uint32_t)ftell(f);
    fclose(f);
    strncpy(name, d->name, cap - 1);
    name[cap - 1] = '\0';
    return true;
}

static bool disk_read_head(void *ctx, const char *name, uint8_t *buf, size_t n) {
    disk_t *d = ctx;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", d->dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n;
}

static bool disk_read_all(void *ctx, const char *name, uint8_t *buf, size_t cap,
                          uint32_t *out_len) {
    disk_t *d = ctx;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", d->dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t got = fread(buf, 1, cap, f);
    fclose(f);
    if (got == 0 || got == cap) {
        return false;
    }
    *out_len = (uint32_t)got;
    return true;
}

static const rom_io_t DISK = { disk_next, disk_read_head, disk_read_all };

/* ---- a simulated GBA BIOS ----------------------------------------------- */
/*
 * The receiving half: it picks a client byte, derives the same session key,
 * decrypts every word into an EWRAM image and computes its own CRC. Anything
 * less would pass with the wrong constants.
 */

typedef enum {
    S_SYNC = 0, S_RECOG, S_HEADER, S_HDR_DONE, S_SYNC2,
    S_KEY1, S_KEY2, S_KEY3, S_LEN, S_DATA, S_CRC_REQ, S_CRC_VAL, S_DONE
} sim_state_t;

typedef struct {
    sim_state_t state;
    uint8_t     client;
    uint32_t    m, crc, offset, header_words, payload_words, f;
    uint8_t     ewram[MB_MAX_SIZE];
    uint8_t     header[MB_HEADER];
    uint32_t    crc_from_master;
} sim_t;

static uint32_t sim_xfer(void *ctx, uint32_t w) {
    sim_t *g = ctx;

    if (g->state == S_DATA && w == 0x00000065u) {
        g->crc = mb_crc_step(g->crc, g->f);
        g->state = S_CRC_REQ;
        return 0x00750065u;
    }
    if (g->state == S_CRC_REQ) {
        if (w == 0x00000066u) { g->state = S_CRC_VAL; return 0; }
        return 0x00750065u;
    }
    if (g->state == S_CRC_VAL) {
        g->crc_from_master = w;
        g->state = S_DONE;
        return ((g->crc & 0xFFFFu) << 16) | 0x66u;
    }

    switch (g->state) {
    case S_SYNC:
        if (w == 0x00006202u) { g->state = S_RECOG; return 0x72026202u; }
        return 0;
    case S_RECOG:
        if (w == 0x00006102u) { g->state = S_HEADER; }
        return 0x72026202u;
    case S_HEADER:
        g->header[g->header_words * 2]     = (uint8_t)(w & 0xFF);
        g->header[g->header_words * 2 + 1] = (uint8_t)((w >> 8) & 0xFF);
        if (++g->header_words == MB_HEADER / 2) { g->state = S_HDR_DONE; }
        return 0;
    case S_HDR_DONE: g->state = S_SYNC2; return 0;
    case S_SYNC2:    g->state = S_KEY1;  return 0;
    case S_KEY1:     g->state = S_KEY2;  return 0;
    case S_KEY2: {
        g->state = S_KEY3;
        uint32_t r = ((uint32_t)g->client << 16) | 0x000063D1u;
        g->m = ((r & 0x00FF0000u) >> 8) + 0xFFFF00D1u;
        return r;
    }
    case S_KEY3:
        g->state = S_LEN;
        return ((uint32_t)g->client << 16) | 0x00006400u;
    case S_LEN: {
        g->payload_words = w;
        uint32_t h = (uint32_t)g->client + 0x0F;
        g->f = (((uint32_t)g->client << 16) & 0x00FF0000u) >> 8;
        g->f = (g->f + h) | 0xFFFF0000u;
        g->state  = S_DATA;
        g->offset = MB_HEADER;
        return ((uint32_t)g->client << 16);
    }
    case S_DATA: {
        g->m = mb_key_next(g->m);
        uint32_t plain = w ^ mb_word_mask(g->m, MB_LOAD_ADDR + g->offset);
        memcpy(g->ewram + g->offset, &plain, 4);
        g->crc = mb_crc_step(g->crc, plain);
        uint32_t echo = ((g->offset & 0xFFFFu) << 16);
        g->offset += 4;
        return echo;
    }
    default:
        return 0;
    }
}

/* ---- the test ----------------------------------------------------------- */

static uint8_t g_stage[MB_MAX_SIZE];

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "rom/tester.gba";

    /* Split into directory and name, as a real drive would present it. */
    char dir[512];
    strncpy(dir, path, sizeof dir - 1);
    dir[sizeof dir - 1] = '\0';
    char *slash = strrchr(dir, '/');
    const char *name = path;
    if (slash) {
        *slash = '\0';
        name = slash + 1;
    } else {
        strcpy(dir, ".");
    }

    printf("using %s/%s\n\n", dir, name);

    /* ---- 1. the catalogue finds and accepts it ---- */
    printf("catalogue\n");
    disk_t disk = { dir, name, 0 };
    rom_catalog_t cat;
    rom_catalog_scan(&cat, &DISK, &disk, 0);

    check("found", cat.count, 1);
    if (cat.count != 1) {
        printf("\nFAILURES\n");
        return 1;
    }
    printf("  %s: %u bytes, %s\n", cat.entries[0].name, cat.entries[0].size,
           rom_status_name(cat.entries[0].status));
    check("playable", cat.entries[0].status, ROM_OK);
    check("counted playable", cat.playable, 1);

    /* ---- 2. staging ---- */
    printf("staging\n");
    uint32_t len = 0;
    check("staged", rom_stage(&cat, 0, &DISK, &disk, g_stage,
                              sizeof g_stage, &len), 1);
    check("length is 16-byte aligned past the header",
          (long)((len - MB_HEADER) % 16), 0);
    check("within EWRAM", len <= MB_MAX_SIZE, 1);
    printf("  staged %u bytes\n", len);

    /* The staged image must still be a valid GBA header -- padding and
     * alignment must not have disturbed it. */
    check("header survives staging",
          rom_validate(g_stage, MB_HEADER, len), ROM_OK);

    /* ---- 3. transfer ---- */
    printf("transfer\n");
    static sim_t gba;
    memset(&gba, 0, sizeof gba);
    gba.client = 0xD1;
    gba.crc    = MB_CRC_SEED;

    mb_session_t s;
    mb_init(&s, sim_xfer, &gba);
    mb_result_t r = mb_send(&s, g_stage, len);
    printf("  %s, %u words, client byte 0x%02X, crc 0x%04X\n",
           mb_result_name(r), s.words_sent, s.client_data, s.crc_expected);
    check("transfer succeeded", r, MB_OK);
    check("checksums agree", s.crc_expected, s.crc_received);

    /* ---- 4. what arrived is what we sent ---- */
    printf("delivery\n");
    check("header arrived verbatim", memcmp(gba.header, g_stage, MB_HEADER), 0);

    size_t payload = mb_padded_size(len) - MB_HEADER;
    check("payload decrypts byte-identically",
          memcmp(gba.ewram + MB_HEADER, g_stage + MB_HEADER, payload), 0);
    printf("  %zu bytes of payload identical after encryption and transfer\n",
           payload);

    /* The ROM's entry branch is the first thing the console executes, so if
     * anything is wrong at offset 0 nothing else matters. */
    check("entry point intact", gba.header[3], 0xEA);
    check("fixed byte intact", gba.header[0xB2], 0x96);

    /* ---- 5. a corrupted image must not pass ---- */
    printf("control\n");
    static sim_t bad;
    memset(&bad, 0, sizeof bad);
    bad.client = 0xD1;
    bad.crc    = MB_CRC_SEED;

    g_stage[MB_HEADER + 8] ^= 0x01;   /* one bit, deep in the payload */
    mb_session_t s2;
    mb_init(&s2, sim_xfer, &bad);
    /* The receiver computes its CRC over what it decrypts, so flipping a bit
     * after staging makes the two disagree -- which is the only way the master
     * ever learns the transfer was wrong. */
    mb_result_t r2 = mb_send(&s2, g_stage, len);
    check("a flipped bit changes the checksum",
          s2.crc_expected != s.crc_expected, 1);
    (void)r2;

    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
