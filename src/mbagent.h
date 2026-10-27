/*
 * mbagent.h -- read and write a GBA's memory through the memory agent.
 *
 * rom/agent.c, once Send Program has started it, answers on the link cable
 * as a Normal-mode SIO slave (rom/agent_proto.h has the protocol). This is
 * the board's side: the same injected word exchange mb_send() uses, so the
 * same SPI transfer drives both, and a host test can drive it against the
 * agent's own core.
 *
 * Every operation checks what came back: the acknowledgement of each
 * header, the echoes, a checksum over every READ and WRITE. On any mismatch
 * it resynchronises (waits out the agent's timeout, clocks until the agent
 * answers IDLE) and tries again, up to `retries` times.
 */
#ifndef MBAGENT_H
#define MBAGENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gba_multiboot.h"   /* mb_xfer_fn */

#ifdef __cplusplus
extern "C" {
#endif

/* The agent's protocol constants, shared with it (rom/agent_proto.h). */
#include "../rom/agent_proto.h"

/* Words per READ or WRITE command: a retry resends at most this much. */
#define MBA_CHUNK 256

typedef enum {
    MBA_OK = 0,
    MBA_ERR_ARGS,       /* misaligned, empty, or NULL                         */
    MBA_ERR_NO_AGENT,   /* nothing answered PING as the agent                 */
    MBA_ERR_DESYNC,     /* replies out of step, still after every retry       */
    MBA_ERR_CHECKSUM,   /* data arrived damaged, still after every retry      */
    MBA_ERR_REFUSED,    /* the agent said no: see mba_client_t.last_status    */
} mba_result_t;

typedef struct {
    mb_xfer_fn xfer;
    /* Wait this long: to let the agent time out a broken command, or a
     * FILL or a CALL run. NULL on a host that does not need to wait. */
    void     (*wait_us)(void *ctx, uint32_t us);
    void      *ctx;
    uint8_t    retries;      /* default 3 */

    /* from PING */
    uint32_t   version;
    uint32_t   free_start;   /* the first EWRAM byte the agent does not use */

    /* observability */
    uint32_t   last_status;  /* the agent's status word for MBA_ERR_REFUSED  */
    uint32_t   words;        /* transfers made                               */
    uint32_t   resyncs;
    uint32_t   retried;
} mba_client_t;

void mba_init(mba_client_t *c, mb_xfer_fn xfer, void (*wait_us)(void *, uint32_t), void *ctx);

/* Is the agent there? Fills version and free_start. */
mba_result_t mba_ping(mba_client_t *c);

/* n 32-bit words from or to addr (4-aligned), split into MBA_CHUNK commands. */
mba_result_t mba_read(mba_client_t *c, uint32_t addr, uint32_t *out, size_t n);
mba_result_t mba_write(mba_client_t *c, uint32_t addr, const uint32_t *in, size_t n);

/* n words of value from addr (4-aligned). */
mba_result_t mba_fill(mba_client_t *c, uint32_t addr, size_t n, uint32_t value);

/* One halfword store, for I/O registers and palette RAM (2-aligned). */
mba_result_t mba_poke16(mba_client_t *c, uint32_t addr, uint16_t value);

/*
 * Call addr (bit 0 set: Thumb) and wait run_us for it to return. The agent
 * is back when a PING answers; this checks that too.
 */
mba_result_t mba_call(mba_client_t *c, uint32_t addr, uint32_t run_us);

const char *mba_result_name(mba_result_t r);
/* The agent's status words as text: "OKAY", "PROT", ... */
const char *mba_status_name(uint32_t status);

#ifdef __cplusplus
}
#endif

#endif /* MBAGENT_H */
