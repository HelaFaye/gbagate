/*
 * link_bridge.h -- ties the wire protocol and the pacing logic to a transport.
 *
 * link_proto.c knows how to frame a packet and link_session.c knows when to
 * stall; neither knows how bytes reach the other end. This is the piece in
 * between: it owns the receive state, runs the HELLO negotiation, routes each
 * packet type, measures round-trip time, and recovers from a peer that
 * disappears and comes back.
 *
 * The transport is a single "send these bytes" callback, so the same object
 * runs over USB CDC (a byte stream, SLIP framed) and over ESP-NOW (datagrams,
 * unframed) with one flag. Both are testable on a host by wiring two bridges
 * back to back.
 *
 * What this is not
 * ----------------
 * It is not reliable delivery. ESP-NOW drops packets and USB does not, and the
 * link layer above copes with both the same way: a word that does not arrive
 * in time becomes an underrun and the slot reads as absent, which is a state
 * the GBA hardware already produces. Retransmission would add latency to fix
 * something that is already handled, so there is none.
 */

#ifndef LINK_BRIDGE_H
#define LINK_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_proto.h"
#include "link_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BRIDGE_DOWN = 0,   /* nothing heard from the peer            */
    BRIDGE_HELLO,      /* we announced ourselves, awaiting a reply */
    BRIDGE_READY       /* both sides agreed; words may flow      */
} bridge_state_t;

/* Send raw bytes. Returns false if the transport refused them. */
typedef bool (*bridge_tx_fn)(void *ctx, const uint8_t *data, size_t len);

/* A completed transfer arriving from the far end. */
typedef void (*bridge_result_fn)(void *ctx, const gba_transfer_t *xfer, uint16_t seq);

/* A word the far end put on the wire, arriving as LP_WORD. Lets a transport
 * hand each inbound word straight to whatever consumes it -- the bridge-backed
 * link_source under QEMU. NULL when nothing needs it, e.g. the host tests. */
typedef void (*bridge_word_fn)(void *ctx, uint16_t seq, uint16_t word);

typedef struct {
    /* configuration */
    bridge_tx_fn     tx;
    bridge_result_fn on_result;
    bridge_word_fn   on_word;
    void            *ctx;
    bool             stream;      /* true: SLIP framing. false: datagrams. */
    gba_role_t       role;
    uint8_t          players;

    /* state */
    bridge_state_t state;
    lp_rx_t        rx;
    link_session_t session;
    uint16_t       next_seq;      /* sequence for our next outgoing word */
    uint32_t       last_rx_us;
    uint32_t       last_hello_us;
    uint32_t       rtt_us;        /* most recent measurement, 0 if none */

    /* counters worth putting on a diagnostics screen */
    uint32_t hellos_sent;
    uint32_t words_sent;
    uint32_t words_received;
    uint32_t results_sent;
    uint32_t results_received;
    uint32_t underruns_reported;
    uint32_t resets;
    uint32_t tx_failures;
    uint32_t bad_packets;
    uint32_t ignored;             /* well-formed but unexpected here */
} link_bridge_t;

/* Retry the announcement this often while the peer is silent. */
#define BRIDGE_HELLO_INTERVAL_US 250000u

/* Declare the peer gone after this much silence. */
#define BRIDGE_TIMEOUT_US 2000000u

void bridge_init(link_bridge_t *b, bridge_tx_fn tx, void *ctx,
                 bool stream, gba_role_t role);

/* Announce ourselves. Safe to call repeatedly; bridge_tick does it for you. */
bool bridge_hello(link_bridge_t *b, uint32_t now_us);

/*
 * Feed bytes from the transport. For a stream this may be any fragment; for a
 * datagram transport, pass exactly one datagram per call.
 */
void bridge_feed(link_bridge_t *b, const uint8_t *data, size_t len, uint32_t now_us);

/* Housekeeping: retry the hello, notice a peer that went away. */
void bridge_tick(link_bridge_t *b, uint32_t now_us);

/* Push our word for a sequence to the peer. */
bool bridge_send_word(link_bridge_t *b, uint16_t seq, uint16_t word);

/* Report a completed transfer to the peer. */
bool bridge_report(link_bridge_t *b, const gba_transfer_t *xfer, uint16_t seq,
                   bool underran);

/* Measure the round trip. The reply updates rtt_us. */
bool bridge_ping(link_bridge_t *b, uint32_t now_us);

/* Drop the session and tell the peer to resynchronise. */
bool bridge_reset(link_bridge_t *b, uint16_t seq);

static inline bool bridge_ready(const link_bridge_t *b) {
    return b->state == BRIDGE_READY;
}

const char *bridge_state_name(bridge_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* LINK_BRIDGE_H */
