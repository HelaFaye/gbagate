/*
 * gbl_peer.h -- a wireless link cable: two gbagate boards, each running
 * GB-Link's adapter firmware, joined by a radio instead of by Celio.
 *
 * GB-Link's online play works because its relay is packet-level, not
 * word-level: the adapter answers the GBA itself and only whole Gen 3 frames
 * (crc + 8 commands, 64 bytes per 4) cross the network, so latency only delays
 * packets and never corrupts the wire. Celio is the coordinator on the other
 * end of the internet -- it picks which adapter is master, tells both to start
 * their handshakes once both GBAs are waiting, and relays "connected" from one
 * side to the other so the master knows when to send 0x8FFF.
 *
 * This module is Celio's session logic (Celio-Server src/session.ts) and its
 * client's packet ordering (Celio-Client LinkExchangeSession), run on each
 * board for that board's own firmware, with the two boards talking to each
 * other directly. No PC, no server, no internet: a link cable with no cable.
 *
 * What it drives, and how
 * -----------------------
 * The local GB-Link firmware, through two injected calls: a command (exactly
 * what a host would send on EP1) and a 64-byte data packet (EP2). Everything
 * the firmware sends back -- status words and data -- is fed in here instead
 * of going to USB. See Transport's local route, PATCHES.md P8.
 *
 * The radio
 * ---------
 * Datagrams, at most GBL_PEER_MAX_DATAGRAM bytes, which may be lost,
 * duplicated or reordered. Celio leans on Socket.IO acks and retries because
 * some Gen 3 words (a trade confirmation) are sent once and never again, so
 * dropping one hangs both games. The same holds here: status and data travel
 * on a reliable, ordered channel (sequence numbers, cumulative acks,
 * go-back-N retransmission), and only HELLO is fire-and-forget.
 *
 * Modes
 * -----
 * Both boards must run the same GB-Link mode and variant: HELLO carries them,
 * and a board in another one is not taken for a peer.
 *
 * Roles
 * -----
 * Celio makes the first adapter to report AwaitMode the master. Two boards
 * have no referee, so each draws a random nonce at boot and the larger is
 * master. The master's GBA is the link child, the slave's GBA the parent --
 * which is what a cable between the two GBAs would have produced.
 */
#ifndef GBL_PEER_H
#define GBL_PEER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBL_PEER_MAX_DATAGRAM 80u   /* header 10 + 64 data + crc 2, rounded */
#define GBL_PEER_WINDOW       16u   /* messages in flight                    */
#define GBL_PEER_QUEUE        64u   /* messages waiting to be sent           */
#define GBL_PEER_RTO_MS       40u   /* retransmit if unacked this long       */
#define GBL_PEER_HELLO_MS     200u
#define GBL_PEER_TIMEOUT_MS   3000u /* peer silent this long: link is down   */
#define GBL_PEER_SETTLE_MS    500u  /* Celio: Cancel, wait 500 ms, SetMode   */
#define GBL_PEER_RESTART_MS   2000u /* Celio: both closed, session ends 2 s on */

typedef enum {
    GBL_PEER_IDLE = 0,    /* not started                                   */
    GBL_PEER_SEARCHING,   /* sending HELLO, no peer heard                  */
    GBL_PEER_SETTLING,    /* peer found, Cancel sent, waiting to SetMode   */
    GBL_PEER_LINKING,     /* mode set; the GBAs are handshaking            */
    GBL_PEER_LINKED,      /* both sides reported LinkConnected             */
    GBL_PEER_CLOSED       /* both sides closed; restarting shortly         */
} gbl_peer_state_t;

typedef struct {
    /* A command for the local firmware, as a host would send it. */
    void (*inject_cmd)(void *ctx, const uint8_t *cmd, size_t len);
    /* A data packet for the local firmware. */
    void (*inject_data)(void *ctx, const uint8_t *data, size_t len);
    /* A datagram for the peer. False if the radio refused it now. */
    bool (*radio_tx)(void *ctx, const uint8_t *dgram, size_t len);
    void *ctx;
} gbl_peer_io_t;

typedef struct {
    uint8_t  type;
    uint8_t  len;
    uint16_t seq;
    uint32_t sent_ms;     /* 0 = not yet sent */
    uint8_t  payload[64];
} gbl_peer_msg_t;

/* What the local firmware's Wireless Adapter mode (0x07) says about its GBA,
 * from the telemetry it sends every 500 ms (rfuProtocolSection.cpp: tag 0x0E
 * detection, 0x1D events). For the link screen; all zero in other modes. */
typedef struct {
    bool     seen;          /* any telemetry yet                           */
    bool     any_rx;        /* a non-zero word ever arrived from the GBA   */
    bool     found;         /* the game ran the ID exchange (comstate >= 2) */
    uint8_t  comstate;
    uint8_t  resets;        /* soft resets the adapter counted (mod 256)   */
    uint32_t transfers;     /* words the GBA clocked                       */
} gbl_peer_rfu_t;

typedef struct {
    gbl_peer_io_t io;

    /* GB-Link mode to run, and its variant byte (0 = none). 0x01 is the
     * Gen 3 relay; 0x04 with 1/2 is Advance Wars. */
    uint8_t mode;
    uint8_t variant;

    gbl_peer_state_t state;
    uint32_t nonce, peer_nonce;
    uint32_t epoch, peer_epoch;   /* bump on restart; stale datagrams drop */
    bool     peer_known;
    bool     peer_hears_us;
    bool     master;
    bool     overflow;             /* the reliable queue filled: restart  */
    uint32_t last_rx_ms, last_hello_ms, state_ms;

    /* Celio's per-client state, for both ends. */
    bool local_hs, peer_hs;        /* FF03 */
    bool local_up, peer_up;        /* FF05 */
    bool local_closed, peer_closed;/* FF07 */

    /* Reliable channel, sender side. */
    gbl_peer_msg_t q[GBL_PEER_QUEUE];
    uint16_t q_head;               /* index of the oldest unacked message */
    uint16_t q_count;
    uint16_t next_seq;             /* seq the next queued message gets    */
    /* Receiver side. */
    uint16_t expect_seq;
    bool     ack_due;

    /* Counters for the link screen. */
    uint32_t tx_datagrams, rx_datagrams, retransmits, bad_datagrams;
    uint32_t data_out, data_in, queue_full;
    uint32_t mode_mismatch;        /* HELLOs from a board in another mode */
    gbl_peer_rfu_t rfu;
} gbl_peer_t;

void gbl_peer_init(gbl_peer_t *p, const gbl_peer_io_t *io, uint32_t nonce,
                   uint8_t mode, uint8_t variant);

/* Begin looking for the peer. */
void gbl_peer_start(gbl_peer_t *p, uint32_t now_ms);

/* Stop: cancel the local mode and forget the peer. */
void gbl_peer_stop(gbl_peer_t *p);

/* A status word the local firmware sent. */
void gbl_peer_on_status(gbl_peer_t *p, uint16_t status, uint32_t now_ms);

/* A data packet the local firmware sent. 64-byte packets cross the cable;
 * the Wireless Adapter's telemetry is noted in p->rfu. */
void gbl_peer_on_data(gbl_peer_t *p, const uint8_t *data, size_t len, uint32_t now_ms);

/* A datagram from the radio. True when it came from a peer: a well-formed
 * HELLO in our mode, or a datagram of the session with the peer we know. A
 * radio that locks onto one sender locks on this, not on the first datagram
 * that looks like ours (a board in another mode would win and never pair). */
bool gbl_peer_on_radio(gbl_peer_t *p, const uint8_t *dgram, size_t len, uint32_t now_ms);

/* Housekeeping: hellos, retransmission, timeouts, delayed steps. */
void gbl_peer_tick(gbl_peer_t *p, uint32_t now_ms);

const char *gbl_peer_state_name(gbl_peer_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* GBL_PEER_H */
