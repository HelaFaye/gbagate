/*
 * mbagent.c -- see mbagent.h.
 *
 * A note on reading the code: Normal mode is full duplex, so x(w) returns
 * the agent's reply to the word sent *before* w. Every exchange below is
 * written as the word sent and, on the same line, what that transfer brings
 * back about the previous one.
 */

#include "mbagent.h"

#include <string.h>

/* What the agent may still need, beyond the 36 us the SPI layer leaves
 * between words, before it can answer again. */
#define RESYNC_US   (AG_TIMEOUT_US + 5000u)
#define FILL_US_PER_WORD 2u     /* the agent stores ~0.8 us a word (agent_emu): twice that */

void mba_init(mba_client_t *c, mb_xfer_fn xfer, void (*wait_us)(void *, uint32_t), void *ctx) {
    memset(c, 0, sizeof *c);
    c->xfer = xfer;
    c->wait_us = wait_us;
    c->ctx = ctx;
    c->retries = 3;
}

static uint32_t x(mba_client_t *c, uint32_t w) {
    c->words++;
    return c->xfer(c->ctx, w);
}

static void wait(mba_client_t *c, uint32_t us) {
    if (c->wait_us && us) {
        c->wait_us(c->ctx, us);
    }
}

/*
 * Back in step: wait out the agent's timeout, so a half-finished command is
 * dropped, then clock until it answers IDLE twice running. True if it did.
 */
static bool resync(mba_client_t *c) {
    c->resyncs++;
    wait(c, RESYNC_US);
    int idle = 0;
    for (int i = 0; i < 8 && idle < 2; i++) {
        idle = x(c, AG_NOP) == AG_IDLE ? idle + 1 : 0;
    }
    return idle >= 2;
}

typedef enum { STEP_OK, STEP_DESYNC, STEP_CHECKSUM, STEP_REFUSED } step_t;

/* Run one attempt, and on a failure resync and try again. */
static mba_result_t attempt(mba_client_t *c, step_t (*op)(mba_client_t *, void *), void *arg) {
    step_t last = STEP_DESYNC;
    for (unsigned t = 0; t <= c->retries; t++) {
        if (t) {
            c->retried++;
            if (!resync(c)) {
                last = STEP_DESYNC;
                continue;
            }
        }
        last = op(c, arg);
        if (last == STEP_OK) return MBA_OK;
        if (last == STEP_REFUSED) return MBA_ERR_REFUSED;   /* asking again changes nothing */
    }
    return last == STEP_CHECKSUM ? MBA_ERR_CHECKSUM : MBA_ERR_DESYNC;
}

/* ---- PING ---------------------------------------------------------------------- */

static step_t op_ping(mba_client_t *c, void *arg) {
    (void)arg;
    const uint32_t h = AG_HDR(AG_PING, 0);
    x(c, h);                                    /* (whatever was staged) */
    if (x(c, AG_NOP) != ~h) return STEP_DESYNC; /* ack of the header     */
    if (x(c, AG_NOP) != AG_ID) return STEP_DESYNC;
    uint32_t v = x(c, AG_NOP);
    uint32_t f = x(c, AG_NOP);
    c->version = v;
    c->free_start = f;
    return STEP_OK;
}

mba_result_t mba_ping(mba_client_t *c) {
    if (!c || !c->xfer) return MBA_ERR_ARGS;
    mba_result_t r = attempt(c, op_ping, NULL);
    return r == MBA_OK ? MBA_OK : MBA_ERR_NO_AGENT;
}

/* ---- READ / WRITE ------------------------------------------------------------------ */

typedef struct {
    uint32_t        addr;
    uint32_t       *out;
    const uint32_t *in;
    uint32_t        n, value, run_us;
    uint16_t        half;
} job_t;

static step_t op_read(mba_client_t *c, void *arg) {
    job_t *j = arg;
    const uint32_t h = AG_HDR(AG_READ, j->n);
    x(c, h);
    if (x(c, j->addr) != ~h) return STEP_DESYNC;
    uint32_t sum = AG_SUM_SEED;
    for (uint32_t i = 0; i < j->n; i++) {
        uint32_t w = x(c, AG_NOP);              /* word i */
        j->out[i] = w;
        sum = ag_sum(sum, w);
    }
    if (x(c, AG_NOP) != sum) return STEP_CHECKSUM;
    return STEP_OK;
}

static step_t op_write(mba_client_t *c, void *arg) {
    job_t *j = arg;
    const uint32_t h = AG_HDR(AG_WRITE, j->n);
    x(c, h);
    if (x(c, j->addr) != ~h) return STEP_DESYNC;
    uint32_t sum = AG_SUM_SEED;
    for (uint32_t i = 0; i < j->n; i++) {
        uint32_t back = x(c, j->in[i]);          /* the address, then words taken */
        if (back != (i == 0 ? j->addr : i)) return STEP_DESYNC;
        sum = ag_sum(sum, j->in[i]);
    }
    if (x(c, sum) != j->n) return STEP_DESYNC;   /* all n taken */
    uint32_t st = x(c, AG_NOP);
    c->last_status = st;
    if (st == AG_ST_OK) return STEP_OK;
    if (st == AG_ST_BADSUM) return STEP_CHECKSUM;
    if (st == AG_ST_PROTECTED || st == AG_ST_ALIGN || st == AG_ST_BADCMD) return STEP_REFUSED;
    return STEP_DESYNC;
}

static mba_result_t chunked(mba_client_t *c, uint32_t addr, uint32_t *out, const uint32_t *in,
                            size_t n, step_t (*op)(mba_client_t *, void *)) {
    if (!c || !c->xfer || n == 0 || (addr & 3) || (!out && !in)) return MBA_ERR_ARGS;
    if ((uint64_t)addr + (uint64_t)n * 4u > 0x100000000ull) return MBA_ERR_ARGS;
    for (size_t done = 0; done < n;) {
        size_t k = n - done > MBA_CHUNK ? MBA_CHUNK : n - done;
        job_t j = { .addr = addr + (uint32_t)done * 4u, .n = (uint32_t)k,
                    .out = out ? out + done : NULL, .in = in ? in + done : NULL };
        mba_result_t r = attempt(c, op, &j);
        if (r != MBA_OK) return r;
        done += k;
    }
    return MBA_OK;
}

mba_result_t mba_read(mba_client_t *c, uint32_t addr, uint32_t *out, size_t n) {
    return chunked(c, addr, out, NULL, n, op_read);
}

mba_result_t mba_write(mba_client_t *c, uint32_t addr, const uint32_t *in, size_t n) {
    return chunked(c, addr, NULL, in, n, op_write);
}

/* ---- FILL / POKE16 / CALL ------------------------------------------------------------ */

static step_t status_step(mba_client_t *c, uint32_t st) {
    c->last_status = st;
    if (st == AG_ST_OK) return STEP_OK;
    if (st == AG_ST_PROTECTED || st == AG_ST_ALIGN || st == AG_ST_BADCMD) return STEP_REFUSED;
    return STEP_DESYNC;
}

static step_t op_fill(mba_client_t *c, void *arg) {
    job_t *j = arg;
    const uint32_t h = AG_HDR(AG_FILL, j->n);
    x(c, h);
    if (x(c, j->addr) != ~h) return STEP_DESYNC;
    if (x(c, j->value) != j->addr) return STEP_DESYNC;   /* the address, echoed */
    wait(c, j->n * FILL_US_PER_WORD);                    /* the agent is storing */
    return status_step(c, x(c, AG_NOP));
}

mba_result_t mba_fill(mba_client_t *c, uint32_t addr, size_t n, uint32_t value) {
    if (!c || !c->xfer || n == 0 || (addr & 3)) return MBA_ERR_ARGS;
    if ((uint64_t)addr + (uint64_t)n * 4u > 0x100000000ull) return MBA_ERR_ARGS;
    for (size_t done = 0; done < n;) {
        size_t k = n - done > AG_MAX_COUNT ? AG_MAX_COUNT : n - done;
        job_t j = { .addr = addr + (uint32_t)done * 4u, .n = (uint32_t)k, .value = value };
        mba_result_t r = attempt(c, op_fill, &j);
        if (r != MBA_OK) return r;
        done += k;
    }
    return MBA_OK;
}

static step_t op_poke16(mba_client_t *c, void *arg) {
    job_t *j = arg;
    const uint32_t h = AG_HDR(AG_POKE16, 0);
    x(c, h);
    if (x(c, j->addr) != ~h) return STEP_DESYNC;
    if (x(c, j->half) != j->addr) return STEP_DESYNC;
    return status_step(c, x(c, AG_NOP));
}

mba_result_t mba_poke16(mba_client_t *c, uint32_t addr, uint16_t value) {
    if (!c || !c->xfer || (addr & 1)) return MBA_ERR_ARGS;
    job_t j = { .addr = addr, .half = value };
    return attempt(c, op_poke16, &j);
}

static step_t op_call(mba_client_t *c, void *arg) {
    job_t *j = arg;
    const uint32_t h = AG_HDR(AG_CALL, 0);
    x(c, h);
    if (x(c, j->addr) != ~h) return STEP_DESYNC;
    step_t s = status_step(c, x(c, AG_NOP));   /* staged before the call began */
    if (s != STEP_OK) return s;
    wait(c, j->run_us);
    return STEP_OK;
}

mba_result_t mba_call(mba_client_t *c, uint32_t addr, uint32_t run_us) {
    if (!c || !c->xfer) return MBA_ERR_ARGS;
    job_t j = { .addr = addr, .run_us = run_us };
    /* Not retried: a call that ran must not run twice. */
    uint8_t keep = c->retries;
    c->retries = 0;
    mba_result_t r = attempt(c, op_call, &j);
    c->retries = keep;
    if (r != MBA_OK) return r;
    return mba_ping(c) == MBA_OK ? MBA_OK : MBA_ERR_NO_AGENT;   /* it came back */
}

const char *mba_result_name(mba_result_t r) {
    switch (r) {
    case MBA_OK:           return "ok";
    case MBA_ERR_ARGS:     return "bad arguments";
    case MBA_ERR_NO_AGENT: return "no agent answered";
    case MBA_ERR_DESYNC:   return "replies out of step";
    case MBA_ERR_CHECKSUM: return "data damaged";
    case MBA_ERR_REFUSED:  return "refused by the agent";
    default:               return "?";
    }
}

const char *mba_status_name(uint32_t st) {
    switch (st) {
    case AG_ST_OK:        return "OKAY";
    case AG_ST_BADSUM:    return "BSUM";
    case AG_ST_PROTECTED: return "PROT";
    case AG_ST_ALIGN:     return "ALGN";
    case AG_ST_BADCMD:    return "BCMD";
    case AG_IDLE:         return "IDLE";
    default:              return "?";
    }
}
