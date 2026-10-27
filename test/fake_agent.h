/*
 * fake_agent.h -- the memory agent services (applet.h) for host tests: the
 * real client (src/mbagent.c) against the agent's real protocol core
 * (rom/agent_proto.h), with a full-duplex lag and a little GBA memory.
 * Included by one test file each; everything is static.
 */
#ifndef FAKE_AGENT_H
#define FAKE_AGENT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint8_t fa_ewram[256 * 1024], fa_pal[1024];

static uint8_t *fa_at(uint32_t a, uint32_t n) {
    if (a >= 0x02000000u && a - 0x02000000u + n <= sizeof fa_ewram) return fa_ewram + (a - 0x02000000u);
    if (a >= 0x05000000u && a - 0x05000000u + n <= sizeof fa_pal) return fa_pal + (a - 0x05000000u);
    return NULL;
}
static uint32_t fa_load32(uint32_t a) { uint32_t v = 0; uint8_t *p = fa_at(a, 4); if (p) memcpy(&v, p, 4); return v; }
static void fa_store32(uint32_t a, uint32_t v) { uint8_t *p = fa_at(a, 4); if (p) memcpy(p, &v, 4); }
static void fa_store16(uint32_t a, uint16_t v) { uint8_t *p = fa_at(a, 2); if (p) memcpy(p, &v, 2); }

#define AG_LOAD32(a)     fa_load32(a)
#define AG_STORE32(a, v) fa_store32((a), (v))
#define AG_STORE16(a, v) fa_store16((a), (uint16_t)(v))
#include "../rom/agent_proto.h"
#include "mbagent.h"

static ag_core_t    fa_core;
static uint32_t     fa_staged;
static mba_client_t fa_client;
static bool         fa_up;
static const char  *fa_fail;      /* what agent_start says, NULL: it works */
static int          fa_starts, fa_stops;

static uint32_t fa_xfer(void *ctx, uint32_t w) {
    (void)ctx;
    uint32_t back = fa_staged;
    fa_staged = ag_step(&fa_core, w);
    return back;
}

static const char *fake_agent_start(void) {
    fa_starts++;
    if (fa_fail) return fa_fail;
    ag_core_init(&fa_core, 0x02000800u);
    ag_protect(&fa_core, 0x02000000u, 0x02000800u);
    fa_staged = AG_IDLE;
    mba_init(&fa_client, fa_xfer, NULL, NULL);
    fa_up = mba_ping(&fa_client) == MBA_OK;
    return fa_up ? NULL : "the agent did not answer";
}
static mba_client_t *fake_agent(void) { return fa_up ? &fa_client : NULL; }
static void fake_agent_stop(void) { fa_stops++; fa_up = false; }

#endif /* FAKE_AGENT_H */
