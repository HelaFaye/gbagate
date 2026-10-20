/*
 * test_frame.c -- the GB-Link serial framing (gblink_frame.c).
 *
 * The vectors marked "upstream" are byte strings GB-Link's own host code
 * produces: gb-pokemon-web SerialConnection.js and the launcher's
 * serial-transport.js frame every command this way, and the ESP32 Switch
 * bridge (GB-Link-Switch-LDN) sends SetMode to the adapter as exactly
 * 47 42 00 03 00 00 07 00.
 */
#include <stdio.h>
#include <string.h>

#include "gblink_frame.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-44s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* Feed bytes; return how many frames completed and keep the last one. */
static int feed(gblink_rx_t *rx, const uint8_t *b, size_t n,
                gblink_frame_t *last, uint8_t *copy) {
    int frames = 0;
    for (size_t i = 0; i < n; i++) {
        gblink_frame_t f;
        if (gblink_rx_feed(rx, b[i], &f)) {
            frames++;
            *last = f;
            if (copy) memcpy(copy, f.payload, f.len);
        }
    }
    return frames;
}

static void test_encode(void) {
    printf("encode\n");
    const uint8_t setmode[] = { 0x00, 0x07, 0x00 };
    const uint8_t want[] = { 0x47, 0x42, 0x00, 0x03, 0x00, 0x00, 0x07, 0x00 };
    uint8_t out[GBLINK_FRAME_MAX];
    size_t n = gblink_frame_encode(GBLINK_CH_COMMAND, setmode, 3, out, sizeof out);
    check("upstream: LDN bridge SetMode length", (long)n, 8);
    check("upstream: LDN bridge SetMode bytes", memcmp(out, want, 8), 0);

    const uint8_t status[] = { 0x08, 0xFF };
    n = gblink_frame_encode(GBLINK_CH_STATUS, status, 2, out, sizeof out);
    check("status frame is 7 bytes", (long)n, 7);
    check("DeviceReady LE on the wire", out[5] == 0x08 && out[6] == 0xFF, 1);

    uint8_t big[65] = { 0 };
    check("65-byte payload refused", (long)gblink_frame_encode(1, big, 65, out, sizeof out), 0);
    check("64-byte payload accepted", (long)gblink_frame_encode(1, big, 64, out, sizeof out), 69);
    check("cap too small refused", (long)gblink_frame_encode(1, big, 4, out, 8), 0);
    check("empty payload is a header", (long)gblink_frame_encode(0, NULL, 0, out, sizeof out), 5);
}

static void test_decode(void) {
    printf("decode\n");
    gblink_rx_t rx;
    gblink_frame_t f;
    uint8_t got[64];

    gblink_rx_init(&rx);
    const uint8_t a[] = { 'G', 'B', 1, 3, 0, 0xAA, 0xBB, 0xCC };
    check("one frame", feed(&rx, a, sizeof a, &f, got), 1);
    check("channel", f.channel, 1);
    check("length", f.len, 3);
    check("payload", got[0] == 0xAA && got[2] == 0xCC, 1);

    /* upstream serialLayer.cpp: a 'G' while waiting for 'B' stays synced. */
    gblink_rx_init(&rx);
    const uint8_t gg[] = { 'G', 'G', 'G', 'B', 0, 1, 0, 0x0F };
    check("GGGB still syncs", feed(&rx, gg, sizeof gg, &f, got), 1);
    check("GGGB payload", got[0], 0x0F);

    /* An oversize length resyncs at once rather than eating 0xFFFF bytes. */
    gblink_rx_init(&rx);
    const uint8_t bad[] = { 'G', 'B', 0, 0xFF, 0xFF, 'G', 'B', 0, 1, 0, 0x42 };
    check("oversize header then good frame", feed(&rx, bad, sizeof bad, &f, got), 1);
    check("oversize counted", (long)rx.oversize, 1);
    check("good frame after it", got[0], 0x42);

    /* Zero-length frames complete on the length byte. */
    gblink_rx_init(&rx);
    const uint8_t z[] = { 'G', 'B', 2, 0, 0 };
    check("zero-length frame", feed(&rx, z, sizeof z, &f, NULL), 1);
    check("zero-length len", f.len, 0);

    /* Modem prober noise between frames. */
    gblink_rx_init(&rx);
    const uint8_t noise[] = "AT+CGMI\r\nATE0\r\nGB\x00\x01\x00\x0f";
    check("AT noise then frame", feed(&rx, noise, sizeof noise - 1, &f, got), 1);
}

static void test_fragmentation(void) {
    printf("every fragmentation\n");
    uint8_t stream[3 * GBLINK_FRAME_MAX];
    size_t n = 0;
    uint8_t p64[64];
    for (int i = 0; i < 64; i++) p64[i] = (uint8_t)(i * 7);
    n += gblink_frame_encode(1, p64, 64, stream + n, sizeof stream - n);
    n += gblink_frame_encode(2, (const uint8_t *)"\x05\xff", 2, stream + n, sizeof stream - n);
    n += gblink_frame_encode(0, (const uint8_t *)"\x0f", 1, stream + n, sizeof stream - n);

    for (size_t step = 1; step <= n; step++) {
        gblink_rx_t rx;
        gblink_rx_init(&rx);
        int frames = 0;
        long sum = 0;
        for (size_t off = 0; off < n; off += step) {
            size_t len = (off + step > n) ? n - off : step;
            for (size_t i = 0; i < len; i++) {
                gblink_frame_t f;
                if (gblink_rx_feed(&rx, stream[off + i], &f)) {
                    frames++;
                    for (int k = 0; k < f.len; k++) sum += f.payload[k];
                }
            }
        }
        if (frames != 3 || sum != 0x0f + 0x05 + 0xff + 7200 /* sum of (i * 7) & 0xFF */) {
            printf("  FAIL fragment size %zu: %d frames, sum %ld\n", step, frames, sum);
            g_fail++;
        }
    }
}

int main(void) {
    test_encode();
    test_decode();
    test_fragmentation();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
