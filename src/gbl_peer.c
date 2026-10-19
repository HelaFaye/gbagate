/*
 * gbl_peer.c -- see gbl_peer.h.
 *
 * Datagram layout (little-endian):
 *
 *   [0] 'G'  [1] 'P'  [2] version  [3] type
 *   [4..7]   sender's epoch
 *   [8..9]   seq of this message (reliable types), else 0
 *   [10..11] ack: the next seq the sender expects from us
 *   [12]     payload length
 *   [13..]   payload
 *   [..+2]   CRC-16/CCITT-FALSE over everything before it
 */
#include "gbl_peer.h"

#include <string.h>

#define VERSION 1u
#define HDR     13u

enum {
    T_HELLO  = 1,  /* nonce u32, epoch of yours I last heard u32            */
    T_STATUS = 2,  /* reliable: a GB-Link status word                      */
    T_DATA   = 3,  /* reliable: a 64-byte GB-Link data packet              */
    T_ACK    = 4   /* nothing but the ack field                            */
};

/* GB-Link status words this follows (linkStatus.hpp). */
#define ST_AWAIT_MODE        0xFF02u
#define ST_HANDSHAKE_RECEIVED 0xFF03u
#define ST_LINK_CONNECTED    0xFF05u
#define ST_LINK_RECONNECTING 0xFF06u
#define ST_LINK_CLOSED       0xFF07u

/* And the commands (control.hpp, link.hpp). */
#define CMD_SET_MODE        0x00u
#define CMD_CANCEL          0x01u
#define CMD_SET_MODE_MASTER 0x10u
#define CMD_SET_MODE_SLAVE  0x11u
#define CMD_START_HANDSHAKE 0x12u
#define CMD_CONNECT_LINK    0x13u

static uint16_t crc16(const uint8_t *d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

/* Sequence comparison that survives wrap: a is before b. */
static bool seq_before(uint16_t a, uint16_t b) { return (int16_t)(a - b) < 0; }

const char *gbl_peer_state_name(gbl_peer_state_t s) {
    switch (s) {
    case GBL_PEER_IDLE:      return "idle";
    case GBL_PEER_SEARCHING: return "searching";
    case GBL_PEER_SETTLING:  return "peer found";
    case GBL_PEER_LINKING:   return "linking";
    case GBL_PEER_LINKED:    return "linked";
    case GBL_PEER_CLOSED:    return "closed";
    }
    return "?";
}

/* ---- local firmware --------------------------------------------------------- */

static void cmd1(gbl_peer_t *p, uint8_t c) { p->io.inject_cmd(p->io.ctx, &c, 1); }

static void set_mode(gbl_peer_t *p) {
    uint8_t c[3] = { CMD_SET_MODE, p->mode, p->variant };
    p->io.inject_cmd(p->io.ctx, c, p->variant ? 3 : 2);
}

static void clear_session(gbl_peer_t *p) {
    p->local_hs = p->peer_hs = false;
    p->local_up = p->peer_up = false;
    p->local_closed = p->peer_closed = false;
}

static void clear_channel(gbl_peer_t *p) {
    p->q_head = p->q_count = 0;
    p->next_seq = 0;
    p->expect_seq = 0;
    p->ack_due = false;
}

/* ---- sending ------------------------------------------------------------------- */

static bool send_raw(gbl_peer_t *p, uint8_t type, uint16_t seq, const uint8_t *pl, uint8_t len) {
    uint8_t d[GBL_PEER_MAX_DATAGRAM];
    d[0] = 'G'; d[1] = 'P'; d[2] = VERSION; d[3] = type;
    put32(d + 4, p->epoch);
    put16(d + 8, seq);
    put16(d + 10, p->expect_seq);
    d[12] = len;
    if (len) memcpy(d + HDR, pl, len);
    put16(d + HDR + len, crc16(d, HDR + len));
    bool ok = p->io.radio_tx(p->io.ctx, d, HDR + len + 2u);
    if (ok) {
        p->tx_datagrams++;
        p->ack_due = false; /* every datagram carries the ack */
    }
    return ok;
}

static void send_hello(gbl_peer_t *p, uint32_t now) {
    uint8_t pl[8];
    put32(pl, p->nonce);
    put32(pl + 4, p->peer_known ? p->peer_epoch : 0);
    send_raw(p, T_HELLO, 0, pl, sizeof pl);
    p->last_hello_ms = now;
}

static void queue_msg(gbl_peer_t *p, uint8_t type, const uint8_t *pl, uint8_t len) {
    if (p->q_count >= GBL_PEER_QUEUE) {
        p->queue_full++;
        return;
    }
    gbl_peer_msg_t *m = &p->q[(p->q_head + p->q_count) % GBL_PEER_QUEUE];
    m->type = type;
    m->len = len;
    m->seq = p->next_seq++;
    m->sent_ms = 0;
    memcpy(m->payload, pl, len);
    p->q_count++;
}

static void queue_status(gbl_peer_t *p, uint16_t s) {
    uint8_t pl[2];
    put16(pl, s);
    queue_msg(p, T_STATUS, pl, 2);
}

/* Send what is due inside the window: new messages, and any whose last send
 * is older than the retransmit timeout. Retransmitting is go-back-N: the
 * receiver drops anything after a gap, so every unacked message is resent. */
static void pump(gbl_peer_t *p, uint32_t now) {
    uint16_t n = p->q_count < GBL_PEER_WINDOW ? p->q_count : (uint16_t)GBL_PEER_WINDOW;
    for (uint16_t i = 0; i < n; i++) {
        gbl_peer_msg_t *m = &p->q[(p->q_head + i) % GBL_PEER_QUEUE];
        bool fresh = m->sent_ms == 0;
        if (!fresh && now - m->sent_ms < GBL_PEER_RTO_MS) continue;
        if (!send_raw(p, m->type, m->seq, m->payload, m->len)) return; /* radio busy */
        if (!fresh) p->retransmits++;
        m->sent_ms = now ? now : 1;
    }
    if (p->ack_due) send_raw(p, T_ACK, 0, NULL, 0);
}

/* Drop everything the peer has acknowledged. */
static void take_ack(gbl_peer_t *p, uint16_t ack) {
    while (p->q_count) {
        gbl_peer_msg_t *m = &p->q[p->q_head];
        if (!seq_before(m->seq, ack)) break;
        p->q_head = (uint16_t)((p->q_head + 1) % GBL_PEER_QUEUE);
        p->q_count--;
    }
}

/* ---- session logic: Celio's, run at both ends ---------------------------------- */

static void maybe_start_handshake(gbl_peer_t *p) {
    /* session.ts: once every client reports HandshakeReceived, both are told
     * StartHandshake. Each board tells its own firmware. */
    if (p->local_hs && p->peer_hs) {
        cmd1(p, CMD_START_HANDSHAKE);
        p->local_hs = p->peer_hs = false; /* once per section */
    }
}

static void begin_session(gbl_peer_t *p, uint32_t now) {
    clear_session(p);
    cmd1(p, CMD_CANCEL);
    p->state = GBL_PEER_SETTLING;
    p->state_ms = now;
}

static void peer_status(gbl_peer_t *p, uint16_t s, uint32_t now) {
    switch (s) {
    case ST_HANDSHAKE_RECEIVED:
        p->peer_hs = true;
        maybe_start_handshake(p);
        break;
    case ST_LINK_CONNECTED:
        /* session.ts: LinkConnected from one side sends ConnectLink to the
         * other -- the master waits for this before sending 0x8FFF. */
        p->peer_up = true;
        cmd1(p, CMD_CONNECT_LINK);
        if (p->local_up) p->state = GBL_PEER_LINKED;
        break;
    case ST_LINK_RECONNECTING:
        p->peer_hs = false;
        p->peer_up = false;
        if (p->state == GBL_PEER_LINKED) p->state = GBL_PEER_LINKING;
        break;
    case ST_LINK_CLOSED:
        p->peer_closed = true;
        if (p->local_closed) { p->state = GBL_PEER_CLOSED; p->state_ms = now; }
        break;
    default:
        break;
    }
}

void gbl_peer_on_status(gbl_peer_t *p, uint16_t s, uint32_t now) {
    if (p->state < GBL_PEER_LINKING) return; /* our own Cancel's echoes */
    switch (s) {
    case ST_AWAIT_MODE:
        cmd1(p, p->master ? CMD_SET_MODE_MASTER : CMD_SET_MODE_SLAVE);
        break;
    case ST_HANDSHAKE_RECEIVED:
        p->local_hs = true;
        queue_status(p, s);
        maybe_start_handshake(p);
        break;
    case ST_LINK_CONNECTED:
        p->local_up = true;
        queue_status(p, s);
        if (p->peer_up) p->state = GBL_PEER_LINKED;
        break;
    case ST_LINK_RECONNECTING:
        p->local_hs = false;
        p->local_up = false;
        queue_status(p, s);
        if (p->state == GBL_PEER_LINKED) p->state = GBL_PEER_LINKING;
        break;
    case ST_LINK_CLOSED:
        p->local_closed = true;
        queue_status(p, s);
        if (p->peer_closed) { p->state = GBL_PEER_CLOSED; p->state_ms = now; }
        break;
    default:
        return;
    }
    pump(p, now);
}

void gbl_peer_on_data(gbl_peer_t *p, const uint8_t *data, size_t len, uint32_t now) {
    if (p->state < GBL_PEER_LINKING || len != 64) return;
    queue_msg(p, T_DATA, data, 64);
    p->data_out++;
    pump(p, now);
}

/* ---- receiving ---------------------------------------------------------------- */

void gbl_peer_on_radio(gbl_peer_t *p, const uint8_t *d, size_t len, uint32_t now) {
    if (p->state == GBL_PEER_IDLE) return;
    if (len < HDR + 2u || d[0] != 'G' || d[1] != 'P' || d[2] != VERSION ||
        d[12] > 64 || len != HDR + d[12] + 2u ||
        get16(d + HDR + d[12]) != crc16(d, HDR + d[12])) {
        p->bad_datagrams++;
        return;
    }
    p->rx_datagrams++;
    p->last_rx_ms = now;

    uint8_t  type  = d[3];
    uint32_t epoch = get32(d + 4);
    uint16_t seq   = get16(d + 8);
    const uint8_t *pl = d + HDR;

    if (type == T_HELLO) {
        uint32_t nonce = get32(pl);
        uint32_t heard = get32(pl + 4);
        if (p->peer_known && epoch != p->peer_epoch) {
            /* The peer restarted. Whatever was in flight belongs to a session
             * it no longer has; start ours over too. */
            clear_channel(p);
            if (p->state >= GBL_PEER_SETTLING) begin_session(p, now);
        }
        p->peer_known = true;
        p->peer_nonce = nonce;
        p->peer_epoch = epoch;
        p->peer_hears_us = heard == p->epoch;
        if (nonce == p->nonce) {
            /* A tie decides nothing; draw again (LCG) and say so. */
            p->nonce = p->nonce * 1103515245u + 12345u;
        }
        /* Answer at once so both sides learn they are heard in one round. */
        if (!p->peer_hears_us || now - p->last_hello_ms >= GBL_PEER_HELLO_MS / 2) send_hello(p, now);
        return;
    }

    if (!p->peer_known || epoch != p->peer_epoch) return; /* stale or stranger */
    take_ack(p, get16(d + 10));

    if (type == T_STATUS || type == T_DATA) {
        p->ack_due = true;
        if (seq != p->expect_seq) return;   /* duplicate, or after a gap */
        p->expect_seq++;
        if (type == T_STATUS && d[12] == 2) {
            peer_status(p, get16(pl), now);
        } else if (type == T_DATA && d[12] == 64) {
            p->io.inject_data(p->io.ctx, pl, 64);
            p->data_in++;
        }
    }
    pump(p, now);
}

/* ---- lifecycle ------------------------------------------------------------------ */

void gbl_peer_init(gbl_peer_t *p, const gbl_peer_io_t *io, uint32_t nonce,
                   uint8_t mode, uint8_t variant) {
    memset(p, 0, sizeof *p);
    p->io = *io;
    p->nonce = nonce ? nonce : 1;
    p->mode = mode;
    p->variant = variant;
    p->epoch = (nonce ^ 0x9E3779B9u) | 1u;
}

static uint32_t mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

void gbl_peer_start(gbl_peer_t *p, uint32_t now) {
    /* A fresh epoch for every start, so the peer can tell a restart -- ours,
     * or a reboot that drew the same nonce -- from the session it knew.
     * Never 0: 0 in a HELLO means "I have not heard you". */
    p->epoch = mix(p->epoch + mix(now) + p->nonce) | 1u;
    p->peer_known = false;
    p->peer_hears_us = false;
    clear_session(p);
    clear_channel(p);
    p->state = GBL_PEER_SEARCHING;
    p->state_ms = now;
    p->last_rx_ms = now;
    send_hello(p, now);
}

void gbl_peer_stop(gbl_peer_t *p) {
    if (p->state != GBL_PEER_IDLE) cmd1(p, CMD_CANCEL);
    p->state = GBL_PEER_IDLE;
    clear_session(p);
    clear_channel(p);
}

void gbl_peer_tick(gbl_peer_t *p, uint32_t now) {
    if (p->state == GBL_PEER_IDLE) return;

    if (now - p->last_hello_ms >= GBL_PEER_HELLO_MS) send_hello(p, now);

    if (p->peer_known && now - p->last_rx_ms >= GBL_PEER_TIMEOUT_MS) {
        /* Peer gone. Stop the local session (a GBA left waiting on a link that
         * cannot finish is better told than left hanging) and look again. */
        if (p->state >= GBL_PEER_SETTLING) cmd1(p, CMD_CANCEL);
        gbl_peer_start(p, now);
        return;
    }

    switch (p->state) {
    case GBL_PEER_SEARCHING:
        if (p->peer_known && p->peer_hears_us && p->nonce != p->peer_nonce) {
            p->master = p->nonce > p->peer_nonce;
            begin_session(p, now);
        }
        break;
    case GBL_PEER_SETTLING:
        if (now - p->state_ms >= GBL_PEER_SETTLE_MS) {
            /* State first: a firmware that answers SetMode synchronously
             * (DeviceReady, AwaitMode from inside the call) must find us
             * already linking, not still settling and deaf to it. */
            p->state = GBL_PEER_LINKING;
            p->state_ms = now;
            set_mode(p);
        }
        break;
    case GBL_PEER_CLOSED:
        if (now - p->state_ms >= GBL_PEER_RESTART_MS) begin_session(p, now);
        break;
    default:
        break;
    }
    pump(p, now);
}
