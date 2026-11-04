/*
 * mp_relay.c -- see mp_relay.h and doc/MULTIPLAY.md.
 */
#include "mp_relay.h"

#include <string.h>

static bool is_handshake(uint16_t w) {
    return (w & ~0x3u) == MP_SLAVE_HANDSHAKE || w == MP_MASTER_HANDSHAKE;
}

static bool q_push(mp_queue_t *q, const uint16_t cmd[MP_CMD_WORDS]) {
    if (q->count >= MP_QUEUE) return false;
    memcpy(q->cmd[(q->head + q->count) % MP_QUEUE], cmd, sizeof q->cmd[0]);
    q->count++;
    return true;
}

static bool q_pop(mp_queue_t *q, uint16_t cmd[MP_CMD_WORDS]) {
    if (!q->count) return false;
    memcpy(cmd, q->cmd[q->head], sizeof q->cmd[0]);
    q->head = (uint8_t)((q->head + 1) % MP_QUEUE);
    q->count--;
    return true;
}

static bool remote(const mp_relay_t *r, unsigned slot) {
    return slot < r->players && slot != r->local;
}

/* A frame starts: each remote player's next command, or idle. */
static void next_frame(mp_relay_t *r) {
    r->pos = 0;
    r->sum = 0;
    memset(r->mine, 0, sizeof r->mine);
    for (unsigned p = 0; p < MP_MAX_PLAYERS; p++) {
        if (!remote(r, p)) continue;
        if (!q_pop(&r->in[p], r->cur[p])) {
            memset(r->cur[p], 0, sizeof r->cur[p]);
            r->idle_given++;
        }
    }
}

void mp_init(mp_relay_t *r, uint8_t players, uint8_t local) {
    memset(r, 0, sizeof *r);
    r->players = players < 2 ? 2 : players > MP_MAX_PLAYERS ? MP_MAX_PLAYERS : players;
    r->local = local < r->players ? local : 0;
    r->phase = MP_HANDSHAKE;
}

void mp_start_handshake(mp_relay_t *r) { r->started = true; }
void mp_connect(mp_relay_t *r) { if (r->local != 0) r->connect = true; }

void mp_tx(mp_relay_t *r, uint16_t out[MP_MAX_PLAYERS]) {
    for (unsigned p = 0; p < MP_MAX_PLAYERS; p++) {
        if (p == r->local) continue;
        if (!remote(r, p)) { out[p] = 0xFFFF; continue; }       /* no unit there */
        switch (r->phase) {
        case MP_HANDSHAKE:
            if (!r->started) out[p] = 0xFFFF;
            /* slot 0 on a child's cable is the parent: it connects once
             * the local GBA has counted every player for a while */
            else if (p == 0 && r->connect && r->ready_rounds >= 2) out[p] = MP_MASTER_HANDSHAKE;
            else out[p] = MP_SLAVE_HANDSHAKE;
            break;
        case MP_CHECKSUM_SLOT:
            out[p] = MP_SLAVE_HANDSHAKE;                           /* unchecked */
            break;
        case MP_FRAMES:
            out[p] = r->pos < MP_CMD_WORDS ? r->cur[p][r->pos] : r->sum;
            break;
        }
    }
}

void mp_done(mp_relay_t *r, const uint16_t w[MP_MAX_PLAYERS]) {
    switch (r->phase) {
    case MP_HANDSHAKE: {
        r->local_ready = is_handshake(w[r->local]);
        bool all = true;
        for (unsigned p = 0; p < r->players; p++) all = all && is_handshake(w[p]);
        r->ready_rounds = all ? (uint8_t)(r->ready_rounds < 255 ? r->ready_rounds + 1 : 255) : 0;
        if (w[0] == MP_MASTER_HANDSHAKE && all) {
            r->connected = true;
            r->connect = false;
            r->phase = MP_CHECKSUM_SLOT;
        }
        break;
    }
    case MP_CHECKSUM_SLOT:
        r->phase = MP_FRAMES;
        next_frame(r);
        break;
    case MP_FRAMES:
        if (r->pos < MP_CMD_WORDS) {
            for (unsigned p = 0; p < r->players; p++) r->sum = (uint16_t)(r->sum + w[p]);
            r->mine[r->pos] = w[r->local];
            r->pos++;
        } else {
            if (w[r->local] != r->sum) r->bad_checksums++;
            bool any = false;
            for (unsigned k = 0; k < MP_CMD_WORDS; k++) any = any || r->mine[k];
            if (any) {
                if (q_push(&r->out, r->mine)) r->relayed++;
                else r->dropped++;
            }
            r->frames++;
            next_frame(r);
        }
        break;
    }
}

bool mp_push(mp_relay_t *r, uint8_t from, const uint16_t cmd[MP_CMD_WORDS]) {
    if (!remote(r, from)) return false;
    if (!q_push(&r->in[from], cmd)) { r->dropped++; return false; }
    r->received++;
    return true;
}

bool mp_pop(mp_relay_t *r, uint16_t cmd[MP_CMD_WORDS]) {
    return q_pop(&r->out, cmd);
}
