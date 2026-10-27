/*
 * agent_proto.h -- the memory agent's protocol: what a GBA running
 * rom/agent.c answers over the link cable, and what src/mbagent.c asks.
 *
 * The agent is a multiboot program. Once Send Program has put it in EWRAM,
 * it stays on the link as a Normal-mode, 32-bit SIO slave, and the board,
 * as master, can read, write, fill and call anything in the GBA's address
 * space. It is the first step toward patching a running game, or feeding it
 * data as it plays.
 *
 * The core below is the whole protocol, freestanding C, compiled into the
 * GBA program and into the host tests alike: what is tested is what runs.
 * The includer defines how memory is touched:
 *
 *     AG_LOAD32(a), AG_STORE32(a, v), AG_STORE16(a, v)
 *
 * The wire
 * --------
 * Normal mode is full duplex: in each 32-bit transfer the master sends a
 * word and receives the one the slave staged after the previous transfer.
 * So the reply to the word sent in transfer i arrives in transfer i + 1, and
 * the master clocks out a reply by sending AG_NOP.
 *
 *     header     AG_MAGIC << 24 | cmd << 16 | count     reply: ~header
 *
 *     PING   n=0  hdr                      -> AG_ID, version, free, AG_IDLE
 *     READ   n    hdr, addr                -> n words, checksum
 *     WRITE  n    hdr, addr, n words, sum  -> status
 *     FILL   n    hdr, addr, value         -> status
 *     POKE16      hdr, addr, value         -> status     (a halfword store)
 *     CALL        hdr, addr                -> status, then the call runs
 *
 * Every address is checked: aligned to its access, and not inside the agent
 * itself (AG_ST_PROTECTED) -- a write over the running agent would end the
 * session in a way the master could not tell from a dropped cable. Status
 * words spell their meaning in ASCII, so a logic analyser trace reads.
 *
 * A master that loses its place (a glitch, a restart mid-command) waits
 * AG_TIMEOUT_US; the agent drops any half-finished command when no transfer
 * arrives for that long, and answers AG_IDLE again.
 */
#ifndef AGENT_PROTO_H
#define AGENT_PROTO_H

#include <stdbool.h>
#include <stdint.h>

#define AG_MAGIC        0xA6u
#define AG_VERSION      1u
#define AG_MAX_COUNT    0xFFFFu
#define AG_TIMEOUT_US   20000u      /* the agent forgets a command after this */

#define AG_HDR(cmd, n)  ((uint32_t)AG_MAGIC << 24 | (uint32_t)(cmd) << 16 | ((uint32_t)(n) & 0xFFFFu))
#define AG_HDR_MAGIC(h) ((uint32_t)(h) >> 24)
#define AG_HDR_CMD(h)   (((uint32_t)(h) >> 16) & 0xFFu)
#define AG_HDR_COUNT(h) ((uint32_t)(h) & 0xFFFFu)

enum {
    AG_NOP = 0,     /* the word sent to clock a reply out; never a header */
    AG_PING = 1,
    AG_READ = 2,
    AG_WRITE = 3,
    AG_FILL = 4,
    AG_POKE16 = 5,
    AG_CALL = 6,
};

#define AG_ID           0x4741424Du   /* "MBAG" read little-endian           */
#define AG_IDLE         0x454C4449u   /* "IDLE": nothing in progress          */
#define AG_ST_OK        0x59414B4Fu   /* "OKAY"                              */
#define AG_ST_BADSUM    0x4D555342u   /* "BSUM": a WRITE's checksum disagreed */
#define AG_ST_PROTECTED 0x544F5250u   /* "PROT": would touch the agent        */
#define AG_ST_ALIGN     0x4E474C41u   /* "ALGN": misaligned for the access    */
#define AG_ST_BADCMD    0x444D4342u   /* "BCMD": unknown command or count     */

/* The checksum over data words: FNV-1a over 32-bit words. Cheap on an
 * ARM7TDMI (one multiply) and it catches a dropped or repeated word, which
 * a plain sum would not. */
#define AG_SUM_SEED     0x811C9DC5u
static inline uint32_t ag_sum(uint32_t sum, uint32_t w) {
    return (sum ^ w) * 0x01000193u;
}

/* ---- the agent's side: compiled where AG_LOAD32 and friends are defined -- */
#ifdef AG_LOAD32

#define AG_RANGES 3

typedef enum {
    AGS_IDLE = 0,
    AGS_PING_OUT,       /* clocking out the PING reply                  */
    AGS_ADDR,           /* the address word of a command is next        */
    AGS_READ_OUT,       /* clocking out data, then the checksum         */
    AGS_WRITE_IN,       /* taking data words                            */
    AGS_WRITE_SUM,      /* the checksum word is next                    */
    AGS_VALUE,          /* FILL / POKE16's value word is next           */
    AGS_STATUS,         /* the status is staged; then idle              */
} ag_state_t;

typedef struct {
    ag_state_t state;
    uint32_t   cmd, count, addr, left, sum, status;
    /* The agent's own bytes, [lo, hi) each: its image, its IWRAM code, its
     * stack. Nothing may be written or called there. */
    uint32_t   protect_lo[AG_RANGES], protect_hi[AG_RANGES];
    uint32_t   ranges;
    uint32_t   free_start;               /* the first byte it does not use */
    uint32_t   call_addr;                /* set by CALL; the host runs it  */
    bool       call_pending;
    uint32_t   commands, errors;         /* for the screen                 */
} ag_core_t;

static inline void ag_core_init(ag_core_t *c, uint32_t free_start) {
    c->state = AGS_IDLE;
    c->cmd = c->count = c->addr = c->left = c->sum = c->status = 0;
    c->ranges = 0;
    c->free_start = free_start;
    c->call_addr = 0;
    c->call_pending = false;
    c->commands = c->errors = 0;
}

/* Mark [lo, hi) as the agent's own. Up to AG_RANGES; more are ignored. */
static inline void ag_protect(ag_core_t *c, uint32_t lo, uint32_t hi) {
    if (c->ranges < AG_RANGES) {
        c->protect_lo[c->ranges] = lo;
        c->protect_hi[c->ranges] = hi;
        c->ranges++;
    }
}

/* Does [a, a + len) touch the agent? len 0 touches nothing. */
static inline bool ag_overlaps(const ag_core_t *c, uint32_t a, uint32_t len) {
    if (len == 0) return false;
    uint32_t end = a + len;
    if (end < a) return true;          /* wraps the address space */
    for (uint32_t i = 0; i < c->ranges; i++) {
        if (a < c->protect_hi[i] && end > c->protect_lo[i]) return true;
    }
    return false;
}

/* A half-finished command is dropped: the master lost its place. */
static inline void ag_timeout(ag_core_t *c) {
    if (c->state != AGS_IDLE) {
        c->errors++;
    }
    c->state = AGS_IDLE;
}

/*
 * One transfer: the word the master just sent in, the word to stage for the
 * next transfer back. The whole protocol is here.
 */
static inline uint32_t ag_step(ag_core_t *c, uint32_t in) {
    switch (c->state) {
    case AGS_IDLE:
        if (AG_HDR_MAGIC(in) != AG_MAGIC) {
            return AG_IDLE;   /* a NOP, or noise: nothing to do */
        }
        c->cmd = AG_HDR_CMD(in);
        c->count = AG_HDR_COUNT(in);
        c->commands++;
        switch (c->cmd) {
        case AG_PING:
            c->state = AGS_PING_OUT;
            c->left = 0;
            break;
        case AG_READ:
        case AG_WRITE:
        case AG_FILL:
            if (c->count == 0) {
                c->status = AG_ST_BADCMD;
                c->state = AGS_STATUS;
                c->errors++;
                return ~in;
            }
            c->state = AGS_ADDR;
            break;
        case AG_POKE16:
        case AG_CALL:
            c->state = AGS_ADDR;
            break;
        default:
            c->status = AG_ST_BADCMD;
            c->state = AGS_STATUS;
            c->errors++;
            break;
        }
        return ~in;   /* the acknowledgement: framed, in step */

    case AGS_PING_OUT:
        switch (c->left++) {
        case 0:  return AG_ID;
        case 1:  return AG_VERSION;
        default: c->state = AGS_IDLE; return c->free_start;
        }

    case AGS_ADDR: {
        c->addr = in;
        c->sum = AG_SUM_SEED;
        uint32_t span = c->cmd == AG_POKE16 ? 2u : c->cmd == AG_CALL ? 0u : c->count * 4u;
        uint32_t align = c->cmd == AG_POKE16 ? 1u : c->cmd == AG_CALL ? 0u : 3u;
        c->status = AG_ST_OK;
        if (in & align) {
            c->status = AG_ST_ALIGN;
        } else if (c->cmd != AG_READ && ag_overlaps(c, in, span)) {
            c->status = AG_ST_PROTECTED;
        } else if (c->cmd == AG_CALL && ag_overlaps(c, in & ~1u, 1)) {
            c->status = AG_ST_PROTECTED;   /* the agent calling itself */
        }
        switch (c->cmd) {
        case AG_READ:
            if (c->status != AG_ST_OK) {
                /* nothing to read: the count's words come back as status,
                 * then a checksum that cannot match, so the master sees it */
                c->errors++;
            }
            c->left = c->count;
            c->state = AGS_READ_OUT;
            {
                uint32_t w = c->status == AG_ST_OK ? AG_LOAD32(c->addr) : c->status;
                c->sum = ag_sum(c->sum, w);
                c->addr += 4;
                c->left--;
                return w;
            }
        case AG_WRITE:
            c->left = c->count;
            c->state = AGS_WRITE_IN;
            return in;   /* the address, echoed */
        case AG_FILL:
        case AG_POKE16:
            c->state = AGS_VALUE;
            return in;
        default:   /* AG_CALL */
            if (c->status == AG_ST_OK) {
                c->call_addr = in;
                c->call_pending = true;
            } else {
                c->errors++;
            }
            c->state = AGS_IDLE;   /* the next word may be a header */
            return c->status;
        }
    }

    case AGS_READ_OUT:
        if (c->left == 0) {
            c->state = AGS_IDLE;
            return c->sum;
        } else {
            uint32_t w = c->status == AG_ST_OK ? AG_LOAD32(c->addr) : c->status;
            c->sum = ag_sum(c->sum, w);
            c->addr += 4;
            c->left--;
            return w;
        }

    case AGS_WRITE_IN:
        if (c->status == AG_ST_OK) {
            AG_STORE32(c->addr, in);
        }
        c->sum = ag_sum(c->sum, in);
        c->addr += 4;
        if (--c->left == 0) {
            c->state = AGS_WRITE_SUM;
        }
        return c->count - c->left;   /* words taken so far */

    case AGS_WRITE_SUM:
        if (c->status == AG_ST_OK && in != c->sum) {
            c->status = AG_ST_BADSUM;
        }
        if (c->status != AG_ST_OK) {
            c->errors++;
        }
        c->state = AGS_IDLE;
        return c->status;

    case AGS_VALUE:
        if (c->status == AG_ST_OK) {
            if (c->cmd == AG_POKE16) {
                AG_STORE16(c->addr, (uint16_t)in);
            } else {
                for (uint32_t i = 0; i < c->count; i++) {
                    AG_STORE32(c->addr + i * 4u, in);
                }
            }
        } else {
            c->errors++;
        }
        c->state = AGS_IDLE;
        return c->status;

    case AGS_STATUS:
    default:
        c->state = AGS_IDLE;
        return c->status;
    }
}

#endif /* AG_LOAD32 */

#endif /* AGENT_PROTO_H */
