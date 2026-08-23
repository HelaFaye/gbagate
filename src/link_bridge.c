/*
 * link_bridge.c -- see link_bridge.h
 */

#include <string.h>

#include "link_bridge.h"

static bool emit(link_bridge_t *b, const lp_packet_t *pkt) {
    uint8_t buf[LP_MAX_FRAMED];
    size_t n = b->stream ? lp_encode(pkt, buf, sizeof buf)
                         : lp_encode_raw(pkt, buf, sizeof buf);
    if (n == 0) {
        b->tx_failures++;
        return false;
    }
    if (!b->tx || !b->tx(b->ctx, buf, n)) {
        b->tx_failures++;
        return false;
    }
    return true;
}

void bridge_init(link_bridge_t *b, bridge_tx_fn tx, void *ctx,
                 bool stream, gba_role_t role) {
    memset(b, 0, sizeof *b);
    b->tx      = tx;
    b->ctx     = ctx;
    b->stream  = stream;
    b->role    = role;
    b->players = 2;
    b->state   = BRIDGE_DOWN;
    lp_rx_init(&b->rx);
    ls_init(&b->session, LS_DEFAULT_STALL_US);
}

bool bridge_hello(link_bridge_t *b, uint32_t now_us) {
    lp_packet_t p;
    memset(&p, 0, sizeof p);
    p.type = LP_HELLO;
    p.seq  = b->session.seq;
    p.u.hello.version = LP_VERSION;
    p.u.hello.role    = (uint8_t)b->role;
    p.u.hello.players = b->players;

    b->hellos_sent++;
    b->last_hello_us = now_us;
    if (b->state == BRIDGE_DOWN) {
        b->state = BRIDGE_HELLO;
    }
    return emit(b, &p);
}

bool bridge_send_word(link_bridge_t *b, uint16_t seq, uint16_t word) {
    if (b->state != BRIDGE_READY) {
        return false;
    }
    lp_packet_t p;
    memset(&p, 0, sizeof p);
    p.type = LP_WORD;
    p.seq  = seq;
    p.u.word.word = word;
    b->words_sent++;
    return emit(b, &p);
}

bool bridge_report(link_bridge_t *b, const gba_transfer_t *xfer, uint16_t seq,
                   bool underran) {
    if (b->state != BRIDGE_READY || !xfer) {
        return false;
    }
    if (underran) {
        lp_packet_t u;
        memset(&u, 0, sizeof u);
        u.type = LP_UNDERRUN;
        u.seq  = seq;
        b->underruns_reported++;
        emit(b, &u);
    }
    lp_packet_t p;
    memset(&p, 0, sizeof p);
    p.type = LP_RESULT;
    p.seq  = seq;
    p.u.result = *xfer;
    b->results_sent++;
    return emit(b, &p);
}

bool bridge_ping(link_bridge_t *b, uint32_t now_us) {
    lp_packet_t p;
    memset(&p, 0, sizeof p);
    p.type = LP_PING;
    p.seq  = b->session.seq;
    p.u.ping.stamp_us = now_us;
    return emit(b, &p);
}

bool bridge_reset(link_bridge_t *b, uint16_t seq) {
    lp_packet_t p;
    memset(&p, 0, sizeof p);
    p.type = LP_RESET;
    p.seq  = seq;
    ls_reset(&b->session, seq);
    b->resets++;
    return emit(b, &p);
}

/* ---- inbound ----------------------------------------------------------- */

static void handle(link_bridge_t *b, const lp_packet_t *p, uint32_t now_us) {
    b->last_rx_us = now_us;

    switch (p->type) {
    case LP_HELLO: {
        /*
         * Answering a HELLO with an ACK rather than another HELLO is what
         * stops two devices that start simultaneously from trading HELLOs
         * forever. Adopt the peer's sequence so both sides agree from the
         * first transfer instead of discovering the mismatch later.
         */
        ls_reset(&b->session, p->seq);
        b->next_seq = p->seq;

        lp_packet_t ack;
        memset(&ack, 0, sizeof ack);
        ack.type = LP_HELLO_ACK;
        ack.seq  = p->seq;
        ack.u.hello.version = LP_VERSION;
        ack.u.hello.role    = (uint8_t)b->role;
        ack.u.hello.players = b->players;
        emit(b, &ack);

        b->state = BRIDGE_READY;
        break;
    }

    case LP_HELLO_ACK:
        if (p->u.hello.version != LP_VERSION) {
            /* A peer speaking a different version is worse than no peer: it
             * would half-work. Stay down and keep announcing. */
            b->ignored++;
            b->state = BRIDGE_HELLO;
            break;
        }
        ls_reset(&b->session, p->seq);
        b->next_seq = p->seq;
        b->state = BRIDGE_READY;
        break;

    case LP_WORD:
        if (b->state != BRIDGE_READY) {
            b->ignored++;
            break;
        }
        b->words_received++;
        ls_push_word(&b->session, p->seq, p->u.word.word);
        /* Deliver to whatever consumes inbound words directly (the gate's
         * link_source under QEMU). The session ring above is for the pacing
         * logic; this is the actual word the gate must answer. */
        if (b->on_word) {
            b->on_word(b->ctx, p->seq, p->u.word.word);
        }
        break;

    case LP_RESULT:
        if (b->state != BRIDGE_READY) {
            b->ignored++;
            break;
        }
        b->results_received++;
        if (b->on_result) {
            b->on_result(b->ctx, &p->u.result, p->seq);
        }
        break;

    case LP_UNDERRUN:
        /* Informational: the far end had to fill a slot. Counted so a
         * diagnostics screen can show which side is struggling. */
        b->underruns_reported++;
        break;

    case LP_RESET:
        ls_reset(&b->session, p->seq);
        b->next_seq = p->seq;
        b->resets++;
        b->state = BRIDGE_READY;
        break;

    case LP_PING: {
        lp_packet_t pong;
        memset(&pong, 0, sizeof pong);
        pong.type = LP_PONG;
        pong.seq  = p->seq;
        pong.u.ping.stamp_us = p->u.ping.stamp_us; /* echo, do not restamp */
        emit(b, &pong);
        break;
    }

    case LP_PONG:
        /* Unsigned subtraction, so a wrapped microsecond counter still gives
         * the right interval. */
        b->rtt_us = now_us - p->u.ping.stamp_us;
        break;

    default:
        b->ignored++;
        break;
    }
}

void bridge_feed(link_bridge_t *b, const uint8_t *data, size_t len,
                 uint32_t now_us) {
    if (!data) {
        return;
    }

    if (!b->stream) {
        lp_packet_t p;
        if (lp_decode_raw(data, len, &p)) {
            handle(b, &p, now_us);
        } else {
            b->bad_packets++;
        }
        return;
    }

    uint32_t dropped_before = b->rx.dropped;
    for (size_t i = 0; i < len; i++) {
        lp_packet_t p;
        if (lp_rx_feed(&b->rx, data[i], &p)) {
            handle(b, &p, now_us);
        }
    }
    b->bad_packets += b->rx.dropped - dropped_before;
}

void bridge_tick(link_bridge_t *b, uint32_t now_us) {
    if (b->state == BRIDGE_READY) {
        if ((uint32_t)(now_us - b->last_rx_us) > BRIDGE_TIMEOUT_US) {
            /* The peer went away. Drop to announcing rather than pretending:
             * a stale session would let stale words look current. */
            b->state = BRIDGE_DOWN;
            ls_reset(&b->session, b->session.seq);
        }
        return;
    }

    if ((uint32_t)(now_us - b->last_hello_us) >= BRIDGE_HELLO_INTERVAL_US ||
        b->hellos_sent == 0) {
        bridge_hello(b, now_us);
    }
}

const char *bridge_state_name(bridge_state_t s) {
    switch (s) {
    case BRIDGE_DOWN:  return "down";
    case BRIDGE_HELLO: return "announcing";
    case BRIDGE_READY: return "ready";
    default:           return "?";
    }
}
