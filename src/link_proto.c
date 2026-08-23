/*
 * link_proto.c -- see link_proto.h
 */

#include <string.h>

#include "link_proto.h"

/* SLIP, RFC 1055. */
#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

#define LP_HDR 3 /* type + seq */

/* ---- CRC-16/CCITT-FALSE ---------------------------------------------- */

static uint16_t lp_crc16(const uint8_t *p, size_t n) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* ---- little-endian helpers ------------------------------------------- */

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)(v & 0xFFFFu));
    put16(p + 2, (uint16_t)(v >> 16));
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

/* ---- payload marshalling --------------------------------------------- */

static size_t lp_payload_len(lp_type_t type) {
    switch (type) {
    case LP_HELLO:
    case LP_HELLO_ACK: return 3;
    case LP_WORD:      return 2;
    case LP_RESULT:    return 10; /* 4 words + players + self_id */
    case LP_UNDERRUN:  return 0;
    case LP_RESET:     return 0;
    case LP_PING:
    case LP_PONG:      return 4;
    default:           return SIZE_MAX; /* unknown */
    }
}

static void lp_put_payload(const lp_packet_t *pkt, uint8_t *p) {
    switch (pkt->type) {
    case LP_HELLO:
    case LP_HELLO_ACK:
        p[0] = pkt->u.hello.version;
        p[1] = pkt->u.hello.role;
        p[2] = pkt->u.hello.players;
        break;
    case LP_WORD:
        put16(p, pkt->u.word.word);
        break;
    case LP_RESULT:
        for (int i = 0; i < 4; i++) {
            put16(p + i * 2, pkt->u.result.word[i]);
        }
        p[8] = pkt->u.result.players;
        p[9] = pkt->u.result.self_id;
        break;
    case LP_PING:
    case LP_PONG:
        put32(p, pkt->u.ping.stamp_us);
        break;
    default:
        break;
    }
}

static void lp_get_payload(lp_packet_t *pkt, const uint8_t *p) {
    memset(&pkt->u, 0, sizeof pkt->u);
    switch (pkt->type) {
    case LP_HELLO:
    case LP_HELLO_ACK:
        pkt->u.hello.version = p[0];
        pkt->u.hello.role    = p[1];
        pkt->u.hello.players = p[2];
        break;
    case LP_WORD:
        pkt->u.word.word = get16(p);
        break;
    case LP_RESULT:
        for (int i = 0; i < 4; i++) {
            pkt->u.result.word[i] = get16(p + i * 2);
        }
        pkt->u.result.players = p[8];
        pkt->u.result.self_id = p[9];
        break;
    case LP_PING:
    case LP_PONG:
        pkt->u.ping.stamp_us = get32(p);
        break;
    default:
        break;
    }
}

/* ---- datagram framing ------------------------------------------------ */

size_t lp_encode_raw(const lp_packet_t *pkt, uint8_t *out, size_t cap) {
    size_t plen = lp_payload_len(pkt->type);
    if (plen == SIZE_MAX) {
        return 0;
    }
    size_t total = LP_HDR + plen + 2;
    if (total > cap || total > LP_MAX_PACKET) {
        return 0;
    }

    out[0] = (uint8_t)pkt->type;
    put16(out + 1, pkt->seq);
    lp_put_payload(pkt, out + LP_HDR);
    put16(out + LP_HDR + plen, lp_crc16(out, LP_HDR + plen));
    return total;
}

bool lp_decode_raw(const uint8_t *in, size_t len, lp_packet_t *out) {
    if (len < LP_HDR + 2) {
        return false;
    }
    lp_type_t type = (lp_type_t)in[0];
    size_t plen = lp_payload_len(type);
    if (plen == SIZE_MAX || len != LP_HDR + plen + 2) {
        return false;
    }
    if (get16(in + LP_HDR + plen) != lp_crc16(in, LP_HDR + plen)) {
        return false;
    }

    out->type = type;
    out->seq  = get16(in + 1);
    lp_get_payload(out, in + LP_HDR);
    return true;
}

/* ---- SLIP framing ---------------------------------------------------- */

size_t lp_encode(const lp_packet_t *pkt, uint8_t *out, size_t cap) {
    uint8_t raw[LP_MAX_PACKET];
    size_t n = lp_encode_raw(pkt, raw, sizeof raw);
    if (n == 0) {
        return 0;
    }

    size_t o = 0;
    /* Leading END flushes any partial frame at the far end after a reset. */
    if (o >= cap) return 0;
    out[o++] = SLIP_END;

    for (size_t i = 0; i < n; i++) {
        uint8_t b = raw[i];
        if (b == SLIP_END || b == SLIP_ESC) {
            if (o + 2 > cap) return 0;
            out[o++] = SLIP_ESC;
            out[o++] = (b == SLIP_END) ? SLIP_ESC_END : SLIP_ESC_ESC;
        } else {
            if (o + 1 > cap) return 0;
            out[o++] = b;
        }
    }

    if (o >= cap) return 0;
    out[o++] = SLIP_END;
    return o;
}

void lp_rx_init(lp_rx_t *rx) {
    memset(rx, 0, sizeof *rx);
}

bool lp_rx_feed(lp_rx_t *rx, uint8_t byte, lp_packet_t *out) {
    if (byte == SLIP_END) {
        bool ok = false;
        if (rx->len > 0 && !rx->overflow) {
            ok = lp_decode_raw(rx->buf, rx->len, out);
            if (!ok) {
                rx->dropped++;
            }
        } else if (rx->overflow) {
            rx->dropped++;
        }
        rx->len      = 0;
        rx->escaped  = false;
        rx->overflow = false;
        return ok;
    }

    if (byte == SLIP_ESC) {
        rx->escaped = true;
        return false;
    }

    if (rx->escaped) {
        rx->escaped = false;
        if (byte == SLIP_ESC_END) {
            byte = SLIP_END;
        } else if (byte == SLIP_ESC_ESC) {
            byte = SLIP_ESC;
        } else {
            rx->overflow = true; /* protocol violation, discard the frame */
            return false;
        }
    }

    if (rx->len >= sizeof rx->buf) {
        rx->overflow = true;
        return false;
    }
    rx->buf[rx->len++] = byte;
    return false;
}

const char *lp_type_name(lp_type_t type) {
    switch (type) {
    case LP_HELLO:     return "HELLO";
    case LP_HELLO_ACK: return "HELLO_ACK";
    case LP_WORD:      return "WORD";
    case LP_RESULT:    return "RESULT";
    case LP_UNDERRUN:  return "UNDERRUN";
    case LP_RESET:     return "RESET";
    case LP_PING:      return "PING";
    case LP_PONG:      return "PONG";
    default:           return "?";
    }
}
