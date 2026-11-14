/*
 * mmo_net.c -- see mmo_net.h.
 */

#include "mmo_net.h"

#include <string.h>

#define END     0xC0
#define ESC     0xDB
#define ESC_END 0xDC
#define ESC_ESC 0xDD

static uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int b = 0; b < 8; b++) {
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
        }
    }
    return c;
}

static size_t put_state(uint8_t *p, const mmo_state_t *s) {
    p[0] = s->group;
    p[1] = s->num;
    p[2] = (uint8_t)s->x;
    p[3] = (uint8_t)((uint16_t)s->x >> 8);
    p[4] = (uint8_t)s->y;
    p[5] = (uint8_t)((uint16_t)s->y >> 8);
    p[6] = s->facing;
    p[7] = s->elevation;
    p[8] = s->gender;
    return 9;
}

static void get_state(const uint8_t *p, mmo_state_t *s) {
    s->group = p[0];
    s->num = p[1];
    s->x = (int16_t)(p[2] | p[3] << 8);
    s->y = (int16_t)(p[4] | p[5] << 8);
    s->facing = p[6];
    s->elevation = p[7];
    s->gender = p[8];
}

/* The unframed message: type, body. Its length, or 0 for an unknown type. */
static size_t body(const mn_msg_t *m, uint8_t *p) {
    p[0] = m->type;
    switch (m->type) {
    case MN_HELLO:   p[1] = m->version; return 2;
    case MN_STATE:   return 1 + put_state(p + 1, &m->st);
    case MN_WELCOME: p[1] = m->pid; p[2] = m->max; return 3;
    case MN_PLAYER:  p[1] = m->pid; return 2 + put_state(p + 2, &m->st);
    case MN_GONE:    p[1] = m->pid; return 2;
    default:         return 0;
    }
}

size_t mn_encode(const mn_msg_t *m, uint8_t *out, size_t cap) {
    uint8_t raw[24];
    size_t n = body(m, raw);
    if (n == 0) {
        return 0;
    }
    uint16_t c = crc16(raw, n);
    raw[n++] = (uint8_t)c;
    raw[n++] = (uint8_t)(c >> 8);

    size_t o = 0;
    if (cap < 1) return 0;
    out[o++] = END;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = raw[i];
        if (b == END || b == ESC) {
            if (o + 2 > cap) return 0;
            out[o++] = ESC;
            out[o++] = b == END ? ESC_END : ESC_ESC;
        } else {
            if (o + 1 > cap) return 0;
            out[o++] = b;
        }
    }
    if (o + 1 > cap) return 0;
    out[o++] = END;
    return o;
}

void mn_rx_init(mn_rx_t *r) {
    memset(r, 0, sizeof *r);
}

static bool decode(const uint8_t *p, size_t n, mn_msg_t *m) {
    if (n < 3) {
        return false;
    }
    uint16_t c = (uint16_t)(p[n - 2] | p[n - 1] << 8);
    if (crc16(p, n - 2) != c) {
        return false;
    }
    n -= 2;
    memset(m, 0, sizeof *m);
    m->type = p[0];
    switch (m->type) {
    case MN_HELLO:   if (n != 2) return false; m->version = p[1]; return true;
    case MN_STATE:   if (n != 10) return false; get_state(p + 1, &m->st); return true;
    case MN_WELCOME: if (n != 3) return false; m->pid = p[1]; m->max = p[2]; return true;
    case MN_PLAYER:  if (n != 11) return false; m->pid = p[1]; get_state(p + 2, &m->st); return true;
    case MN_GONE:    if (n != 2) return false; m->pid = p[1]; return true;
    default:         return false;
    }
}

bool mn_rx_byte(mn_rx_t *r, uint8_t byte, mn_msg_t *out) {
    if (byte == END) {
        bool ok = false;
        if (r->len && !r->overflow) {
            ok = decode(r->buf, r->len, out);
            if (!ok) r->bad++;
        } else if (r->overflow) {
            r->bad++;
        }
        r->len = 0;
        r->esc = false;
        r->overflow = false;
        return ok;
    }
    if (byte == ESC) {
        r->esc = true;
        return false;
    }
    if (r->esc) {
        byte = byte == ESC_END ? END : byte == ESC_ESC ? ESC : byte;
        r->esc = false;
    }
    if (r->len >= sizeof r->buf) {
        r->overflow = true;
        return false;
    }
    r->buf[r->len++] = byte;
    return false;
}
