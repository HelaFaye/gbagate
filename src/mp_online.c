/*
 * mp_online.c -- see mp_online.h.
 */
#include "mp_online.h"

#include <string.h>

void mp_online_init(mp_online_t *m, const mp_online_io_t *io, uint8_t players, uint8_t gbas) {
    memset(m, 0, sizeof *m);
    m->io = *io;
    m->players = players;
    m->gbas = gbas;
    m->first = -1;
}

static void take(mp_online_t *m) { if (m->io.lock) m->io.lock(m->io.ctx); }
static void give(mp_online_t *m) { if (m->io.unlock) m->io.unlock(m->io.ctx); }

static void stop(mp_online_t *m) {
    if (m->running && m->io.bus_stop) m->io.bus_stop(m->io.ctx);
    m->running = false;
}

static void set_role(mp_online_t *m, int first) {
    const mp_bus_layout_t l = { m->players, m->gbas, (uint8_t)first };
    if (!mp_bus_layout_ok(&l)) return;           /* a role our chain cannot take */
    if (m->running && m->first == first) return; /* asked again: keep the link */
    stop(m);
    mp_link_init(&m->link, &l, m->io.lock, m->io.unlock, m->io.ctx);
    m->first = first;
    m->said_handshake = m->said_connected = false;
    m->running = m->io.bus_start && m->io.bus_start(m->io.ctx, &l, &m->link);
}

void mp_online_command(mp_online_t *m, uint8_t players, uint8_t command, int link_id) {
    if (players >= 2 && players <= 4) m->players = players;
    switch (command) {
    case MPO_CMD_SET_SLAVE:
        set_role(m, 0);
        break;
    case MPO_CMD_SET_MASTER:
        if (link_id > 0) set_role(m, link_id);
        break;
    case MPO_CMD_START_HANDSHAKE:
        if (m->first < 0) break;
        take(m);
        mp_start_handshake(&m->link.relay);
        give(m);
        break;
    case MPO_CMD_CONNECT_LINK:
        if (m->first < 0) break;
        take(m);
        mp_connect(&m->link.relay);
        give(m);
        break;
    case MPO_CMD_CANCEL:
        stop(m);
        m->first = -1;
        break;
    default:
        break;
    }
}

void mp_online_data(mp_online_t *m, uint8_t from, const uint8_t d[64]) {
    (void)from;   /* the entries say whose each command is */
    if (m->first < 0) return;
    const unsigned n = d[1];
    bool ok = d[0] == MPO_MAGIC && n >= 1 && n <= MPO_PER_PKT;
    for (size_t i = 2 + 17u * n; ok && i < 64; i++) ok = d[i] == 0;
    const uint8_t mine = m->link.relay.local_mask;
    for (unsigned e = 0; ok && e < n; e++) {
        const uint8_t slot = d[2 + 17 * e];
        ok = slot < m->players && !((mine >> slot) & 1u);
    }
    if (!ok) {
        m->bad_packets++;
        return;
    }
    take(m);
    for (unsigned e = 0; e < n; e++) {
        const uint8_t *p = d + 2 + 17 * e;
        uint16_t cmd[MP_CMD_WORDS];
        for (int k = 0; k < MP_CMD_WORDS; k++) cmd[k] = (uint16_t)(p[1 + 2 * k] | p[2 + 2 * k] << 8);
        mp_push(&m->link.relay, p[0], cmd);
        m->commands_in++;
    }
    give(m);
}

void mp_online_tick(mp_online_t *m) {
    if (!m->running) return;
    take(m);
    const mp_relay_t *r = &m->link.relay;
    const bool hs = r->local_ready && !m->said_handshake;
    const bool connected = m->first == 0 && r->connected && !m->said_connected;
    uint8_t pkt[64];
    unsigned n = 0;
    memset(pkt, 0, sizeof pkt);
    uint8_t from;
    uint16_t cmd[MP_CMD_WORDS];
    while (n < MPO_PER_PKT && mp_pop(&m->link.relay, &from, cmd)) {
        uint8_t *p = pkt + 2 + 17 * n++;
        p[0] = from;
        for (int k = 0; k < MP_CMD_WORDS; k++) { p[1 + 2 * k] = (uint8_t)cmd[k]; p[2 + 2 * k] = (uint8_t)(cmd[k] >> 8); }
    }
    give(m);
    if (hs) { m->said_handshake = true; if (m->io.status) m->io.status(m->io.ctx, MPO_ST_HANDSHAKE_RECEIVED); }
    if (connected) { m->said_connected = true; if (m->io.status) m->io.status(m->io.ctx, MPO_ST_LINK_CONNECTED); }
    if (n) {
        pkt[0] = MPO_MAGIC;
        pkt[1] = (uint8_t)n;
        if (m->io.data_out) m->io.data_out(m->io.ctx, pkt);
        m->packets_out++;
    }
}
