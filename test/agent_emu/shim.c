/*
 * shim.c -- src/mbagent.c with a flat interface, for agent_emu.py to load
 * through ctypes. One client; the transfer and the wait are Python's.
 */
#include "mbagent.h"

static mba_client_t g;

void t_init(mb_xfer_fn xfer, void (*wait_us)(void *, uint32_t)) { mba_init(&g, xfer, wait_us, NULL); }
int t_ping(uint32_t *version, uint32_t *free_start) {
    int r = mba_ping(&g);
    *version = g.version;
    *free_start = g.free_start;
    return r;
}
int t_read(uint32_t a, uint32_t *out, uint32_t n)        { return mba_read(&g, a, out, n); }
int t_write(uint32_t a, const uint32_t *in, uint32_t n)  { return mba_write(&g, a, in, n); }
int t_fill(uint32_t a, uint32_t n, uint32_t v)           { return mba_fill(&g, a, n, v); }
int t_poke16(uint32_t a, uint16_t v)                     { return mba_poke16(&g, a, v); }
int t_call(uint32_t a, uint32_t us)                      { return mba_call(&g, a, us); }
uint32_t t_status(void)  { return g.last_status; }
uint32_t t_resyncs(void) { return g.resyncs; }
uint32_t t_words(void)   { return g.words; }
