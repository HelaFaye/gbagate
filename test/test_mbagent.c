/*
 * test_mbagent.c -- the board's memory client (src/mbagent.c) against the
 * agent's own protocol core (rom/agent_proto.h), over a simulated link with
 * the one-word lag of Normal mode, on a simulated GBA memory map.
 *
 * Then the faults a cable has: a bit flipped either way, a transfer lost,
 * the board restarting mid-command. Each must end in the right memory and
 * a true result -- never a write that half-landed and reported success.
 * test/agent_emu.py runs the same client against the compiled agent on an
 * emulated ARM7.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a GBA memory map ------------------------------------------------------- */

static uint8_t ewram[256 * 1024], iwram[32 * 1024], pal[1024], vram[96 * 1024], io[1024];
/* referenced through at()'s table */

static uint8_t *at(uint32_t a, uint32_t n) {
    struct { uint32_t base, size; uint8_t *mem; } map[] = {
        { 0x02000000, sizeof ewram, ewram }, { 0x03000000, sizeof iwram, iwram },
        { 0x04000000, sizeof io, io },       { 0x05000000, sizeof pal, pal },
        { 0x06000000, sizeof vram, vram },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (a >= map[i].base && a - map[i].base + n <= map[i].size) {
            return map[i].mem + (a - map[i].base);
        }
    }
    return NULL;
}

static uint32_t load32(uint32_t a) {
    uint8_t *p = at(a, 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;   /* open bus reads as 0 here */
}
static void store32(uint32_t a, uint32_t v) { uint8_t *p = at(a, 4); if (p) memcpy(p, &v, 4); }
static void store16(uint32_t a, uint16_t v) { uint8_t *p = at(a, 2); if (p) memcpy(p, &v, 2); }

#define AG_LOAD32(a)     load32(a)
#define AG_STORE32(a, v) store32((a), (v))
#define AG_STORE16(a, v) store16((a), (uint16_t)(v))
#include "../rom/agent_proto.h"
#include "mbagent.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-52s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* ---- the link ------------------------------------------------------------------ */

#define AGENT_END 0x02000800u   /* the agent's own bytes: [0x02000000, here) */

typedef struct {
    ag_core_t core;
    uint32_t  staged;        /* what the agent clocks out next              */
    uint32_t  n;             /* transfers so far                            */
    long      flip_out_at;   /* transfer whose board->agent word is damaged */
    long      flip_in_at;    /* ... agent->board                            */
    long      drop_at;       /* ... lost: the agent never sees it           */
    uint32_t  calls;
    bool      present;       /* a GBA running the agent at all              */
} link_t;

static link_t L;

static void link_reset(bool present) {
    memset(&L, 0, sizeof L);
    ag_core_init(&L.core, AGENT_END);
    ag_protect(&L.core, 0x02000000u, AGENT_END);
    ag_protect(&L.core, 0x03000000u, 0x03000400u);   /* its IWRAM code */
    ag_protect(&L.core, 0x03007E00u, 0x03008000u);   /* its stack */
    L.staged = AG_IDLE;
    L.flip_out_at = L.flip_in_at = L.drop_at = -1;
    L.present = present;
}

static uint32_t xfer(void *ctx, uint32_t w) {
    (void)ctx;
    long i = L.n++;
    if (!L.present) return 0xFFFFFFFFu;   /* nothing drives SI: it floats high */
    if (i == L.drop_at) return L.staged;  /* a glitch: the agent saw no clock */
    if (i == L.flip_out_at) w ^= 1u << 7;
    uint32_t back = L.staged;
    L.staged = ag_step(&L.core, w);
    if (L.core.call_pending) {
        L.core.call_pending = false;
        L.calls++;
    }
    if (i == L.flip_in_at) back ^= 1u << 20;
    return back;
}

static void wait_us(void *ctx, uint32_t us) {
    (void)ctx;
    if (us >= AG_TIMEOUT_US) {
        /* agent.c's main loop: a command left half-done is dropped */
        if (L.core.state != AGS_IDLE) {
            ag_timeout(&L.core);
            L.staged = AG_IDLE;
        }
    }
}

static mba_client_t C;

static void fresh(bool present) {
    link_reset(present);
    memset(ewram, 0, sizeof ewram);
    memset(pal, 0, sizeof pal);
    mba_init(&C, xfer, wait_us, NULL);
}

/* ---- the operations ---------------------------------------------------------- */

static void test_ping(void) {
    fresh(true);
    check("the agent answers", mba_ping(&C), MBA_OK);
    check("... its version", C.version, AG_VERSION);
    check("... where free EWRAM starts", C.free_start, AGENT_END);
    check("five transfers", C.words, 5);
    fresh(false);
    check("no GBA: no agent", mba_ping(&C), MBA_ERR_NO_AGENT);
}

static void test_read_write(void) {
    fresh(true);
    static uint32_t src[700], got[700];
    for (int i = 0; i < 700; i++) src[i] = 0xC0DE0000u + (uint32_t)i * 2654435761u;
    check("write 700 words (three chunks)", mba_write(&C, 0x02010000, src, 700), MBA_OK);
    check("... landed", memcmp(ewram + 0x10000, src, sizeof src), 0);
    check("read them back", mba_read(&C, 0x02010000, got, 700), MBA_OK);
    check("... the same", memcmp(got, src, sizeof src), 0);
    check("no retries on a clean line", C.retried, 0);
    uint32_t per = C.words;
    check("~one transfer per word", per < 2 * 700 + 60, 1);

    check("a misaligned read", mba_read(&C, 0x02010002, got, 1), MBA_ERR_ARGS);
    check("an empty write", mba_write(&C, 0x02010000, src, 0), MBA_ERR_ARGS);
    check("past the top of memory", mba_read(&C, 0xFFFFFFFCu, got, 2), MBA_ERR_ARGS);

    uint32_t before = load32(0x02000100);
    check("a write into the agent is refused", mba_write(&C, 0x020007F8, src, 4), MBA_ERR_REFUSED);
    check("... as PROT", C.last_status, AG_ST_PROTECTED);
    check("... and nothing written", load32(0x02000100) == before && load32(0x02000800) == 0u, 1);
    check("the agent can still be read", mba_read(&C, 0x02000000, got, 4), MBA_OK);
    check("then it carries on", mba_ping(&C), MBA_OK);
}

static void test_fill_poke_call(void) {
    fresh(true);
    check("fill VRAM", mba_fill(&C, 0x06000000, 1000, 0x7FFF7FFF), MBA_OK);
    check("... every word", load32(0x06000000) == 0x7FFF7FFF && load32(0x06000000 + 999 * 4) == 0x7FFF7FFF &&
                            load32(0x06000000 + 1000 * 4) == 0, 1);
    check("poke the backdrop colour", mba_poke16(&C, 0x05000000, 0x03E0), MBA_OK);
    check("... a halfword", pal[0] == 0xE0 && pal[1] == 0x03 && pal[2] == 0, 1);
    check("an odd poke", mba_poke16(&C, 0x05000001, 1), MBA_ERR_ARGS);
    check("fill over the agent", mba_fill(&C, 0x02000400, 4, 0), MBA_ERR_REFUSED);
    check("a write over its IWRAM code is refused", mba_fill(&C, 0x030003FC, 2, 0), MBA_ERR_REFUSED);
    check("... and over its stack", mba_poke16(&C, 0x03007F00, 0), MBA_ERR_REFUSED);
    check("call", mba_call(&C, 0x03004001, 100), MBA_OK);
    check("... ran once", L.calls, 1);
    check("calling the agent is refused", mba_call(&C, 0x02000100, 100), MBA_ERR_REFUSED);
    check("... and did not run", L.calls, 1);
}

static void test_faults(void) {
    static uint32_t src[300], got[300];
    for (int i = 0; i < 300; i++) src[i] = 0x5A000000u | (uint32_t)i;

    /* every single transfer of a write, damaged on the way out */
    long n_clean;
    fresh(true);
    mba_write(&C, 0x02020000, src, 300);
    n_clean = (long)C.words;
    int bad = 0;
    for (long k = 0; k < n_clean; k++) {
        fresh(true);
        L.flip_out_at = k;
        mba_result_t r = mba_write(&C, 0x02020000, src, 300);
        if (r != MBA_OK || memcmp(ewram + 0x20000, src, sizeof src) != 0) {
            if (bad++ < 3) printf("  FAIL a write with transfer %ld damaged out: %s\n", k, mba_result_name(r));
        }
    }
    check("every damaged outgoing word is caught and redone", bad, 0);

    bad = 0;
    for (long k = 0; k < n_clean; k++) {
        fresh(true);
        L.flip_in_at = k;
        mba_result_t r = mba_write(&C, 0x02020000, src, 300);
        if (r != MBA_OK || memcmp(ewram + 0x20000, src, sizeof src) != 0) {
            if (bad++ < 3) printf("  FAIL a write with reply %ld damaged: %s\n", k, mba_result_name(r));
        }
    }
    check("every damaged reply is caught and redone", bad, 0);

    bad = 0;
    for (long k = 0; k < n_clean; k++) {
        fresh(true);
        L.drop_at = k;
        mba_result_t r = mba_write(&C, 0x02020000, src, 300);
        if (r != MBA_OK || memcmp(ewram + 0x20000, src, sizeof src) != 0) {
            if (bad++ < 3) printf("  FAIL a write with transfer %ld lost: %s\n", k, mba_result_name(r));
        }
    }
    check("every lost transfer is caught and redone", bad, 0);

    /* reads: a damaged reply is a checksum failure, retried */
    fresh(true);
    memcpy(ewram + 0x20000, src, sizeof src);
    mba_read(&C, 0x02020000, got, 300);
    n_clean = (long)C.words;
    bad = 0;
    for (long k = 0; k < n_clean; k++) {
        fresh(true);
        memcpy(ewram + 0x20000, src, sizeof src);
        L.flip_in_at = k;
        memset(got, 0, sizeof got);
        mba_result_t r = mba_read(&C, 0x02020000, got, 300);
        if (r != MBA_OK || memcmp(got, src, sizeof src) != 0) {
            if (bad++ < 3) printf("  FAIL a read with reply %ld damaged: %s\n", k, mba_result_name(r));
        }
    }
    check("every damaged read is caught and redone", bad, 0);

    /* the board restarts mid-write: the agent is left waiting for data */
    fresh(true);
    xfer(NULL, AG_HDR(AG_WRITE, 50));
    xfer(NULL, 0x02030000);
    xfer(NULL, 0x11111111);   /* one word in, 49 owed */
    mba_init(&C, xfer, wait_us, NULL);
    check("after a restart mid-write, PING still works", mba_ping(&C), MBA_OK);
    check("... by resynchronising", C.resyncs >= 1, 1);
    check("and the next write lands whole", mba_write(&C, 0x02030000, src, 50), MBA_OK);
    check("... checked", memcmp(ewram + 0x30000, src, 50 * 4), 0);

    /* a line that is always noisy gives up honestly */
    fresh(true);
    C.retries = 2;
    for (int k = 0; k < 3; k++) { (void)k; }
    L.flip_in_at = 2;
    mba_result_t r = mba_read(&C, 0x02020000, got, 4);
    check("one bad reply is retried away", r, MBA_OK);
}

/* a line damaged on every attempt: the result says so */
static uint32_t xfer_noisy(void *ctx, uint32_t w) {
    uint32_t back = xfer(ctx, w);
    return (L.n % 7 == 3) ? back ^ 0x10u : back;
}

static void test_hopeless(void) {
    fresh(true);
    C.xfer = xfer_noisy;
    static uint32_t got[64];
    mba_result_t r = mba_read(&C, 0x02020000, got, 64);
    check("a hopeless line fails", r != MBA_OK, 1);
    check("... after its retries", C.retried, 3);
    check("names", strcmp(mba_status_name(AG_ST_PROTECTED), "PROT") || strcmp(mba_result_name(MBA_ERR_REFUSED), "refused by the agent"), 0);
}

int main(void) {
    test_ping();
    test_read_write();
    test_fill_poke_call();
    test_faults();
    test_hopeless();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
