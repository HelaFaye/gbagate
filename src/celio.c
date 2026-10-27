/*
 * celio.c -- see celio.h.
 */

#include "celio.h"

#include <stdio.h>
#include <string.h>

#include "json_lite.h"

#define CONNECT_MS    10000   /* to the Socket.IO handshake                    */
#define ENTER_MS       5000   /* for the create/join reply                     */
#define CANCEL_MS       500   /* LinkDeviceUtils.tryEnableLinkMode's pause     */
#define READY_MS       2500   /* ... and its wait for DeviceReady              */

#define ST_DEVICE_READY 0xFF08
#define ST_TRADE_DONE   0xFF09
#define ST_DEBUG        0xFFFF

#define CMD_SETMODE 0x00
#define CMD_CANCEL  0x01

static bool due(uint32_t now, uint32_t when) {
    return (int32_t)(now - when) >= 0;
}

static void send(celio_t *c, const char *text, size_t len) {
    if (c->io.ws_send && c->io.ws_send(c->io.ctx, text, len)) {
        c->sent++;
    } else {
        c->dropped++;
    }
}

static void send_str(celio_t *c, const char *text) {
    send(c, text, strlen(text));
}

static void command(celio_t *c, const uint8_t *cmd, size_t len) {
    if (c->io.gbl_command) {
        c->io.gbl_command(c->io.ctx, cmd, len);
    }
}

static void cancel_adapter(celio_t *c) {
    const uint8_t cmd = CMD_CANCEL;
    command(c, &cmd, 1);
}

static void make_uuid(celio_t *c, char out[37]) {
    uint8_t b[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = c->io.random ? c->io.random(c->io.ctx) : 0x12345678u * (uint32_t)(i + 1);
        b[i] = (uint8_t)r; b[i + 1] = (uint8_t)(r >> 8);
        b[i + 2] = (uint8_t)(r >> 16); b[i + 3] = (uint8_t)(r >> 24);
    }
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);   /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);   /* RFC 4122 variant */
    static const char hx[] = "0123456789abcdef";
    size_t o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[o++] = '-';
        }
        out[o++] = hx[b[i] >> 4];
        out[o++] = hx[b[i] & 15];
    }
    out[o] = '\0';
}

static void fail(celio_t *c, const char *why) {
    if (c->state == CELIO_STARTING || c->state == CELIO_LINKED) {
        cancel_adapter(c);
    }
    snprintf(c->error, sizeof c->error, "%s", why);
    c->state = CELIO_FAILED;
}

void celio_start(celio_t *c, const celio_io_t *io, const char *session, uint32_t now_ms) {
    memset(c, 0, sizeof *c);
    c->io = *io;
    make_uuid(c, c->client_id);
    if (session && *session) {
        snprintf(c->session, sizeof c->session, "%s", session);
        c->joining = true;
    }
    c->state = CELIO_CONNECTING;
    c->deadline = now_ms + CONNECT_MS;
}

void celio_stop(celio_t *c) {
    if (c->connected && c->state >= CELIO_WAITING && c->state <= CELIO_LINKED) {
        send_str(c, "42[\"sessionLeft\"]");
    }
    if (c->state == CELIO_STARTING || c->state == CELIO_LINKED) {
        cancel_adapter(c);
    }
    c->state = CELIO_IDLE;
    c->connected = false;
}

void celio_on_open(celio_t *c, uint32_t now_ms) {
    /* Nothing yet: the Engine.IO open packet follows, and the socket counts
     * as back only once the namespace is (lost_since stays until then). */
    (void)c;
    (void)now_ms;
}

void celio_on_close(celio_t *c, uint32_t now_ms) {
    c->connected = false;
    if (c->lost_since == 0) {
        c->lost_since = now_ms ? now_ms : 1;
    }
    if (c->state == CELIO_ENTERING) {
        /* The reply to create/join is lost with the socket, and a retry
         * cannot tell a session made from one not: begin again instead. */
        fail(c, "connection lost");
    }
}

/* ---- Socket.IO framing --------------------------------------------------- */

bool sio_parse(const char *t, size_t n, sio_packet_t *p) {
    memset(p, 0, sizeof *p);
    if (n == 0) {
        return false;
    }
    p->eio = t[0];
    size_t i = 1;
    if (p->eio == '4') {
        if (i >= n) {
            return false;
        }
        p->sio = t[i++];
        if (p->sio < '0' || p->sio > '6') {
            return false;
        }
        if (p->sio == '5' || p->sio == '6') {
            return false;   /* binary event or ack: Celio sends none */
        }
        if (i < n && t[i] == '/') {
            /* a namespace: only the default one is ours */
            size_t s = i;
            while (i < n && t[i] != ',') i++;
            if (!(i - s == 1)) {
                return false;
            }
            if (i < n) i++;
        }
        if (i < n && t[i] >= '0' && t[i] <= '9') {
            uint64_t id = 0;
            while (i < n && t[i] >= '0' && t[i] <= '9') {
                id = id * 10 + (uint64_t)(t[i] - '0');
                if (id > 0xFFFFFFFFu) {
                    return false;
                }
                i++;
            }
            p->has_id = true;
            p->id = (uint32_t)id;
        }
    }
    p->json = t + i;
    p->json_len = n - i;
    return true;
}

static void ack(celio_t *c, uint32_t id) {
    char f[24];
    int l = snprintf(f, sizeof f, "43%lu[true]", (unsigned long)id);
    send(c, f, (size_t)l);
}

/* ---- the session ----------------------------------------------------------- */

static void enter_session(celio_t *c, uint32_t now) {
    char f[96];
    int l;
    c->enter_ack = c->next_ack++;
    if (c->joining) {
        char q[CELIO_SESSION_MAX * 6 + 3];
        json_quote(q, sizeof q, c->session);
        l = snprintf(f, sizeof f, "42%lu[\"sessionJoin\",%s]", (unsigned long)c->enter_ack, q);
    } else {
        /* Celio-Client emits ("sessionCreate", undefined, cb): undefined is
         * serialised as null */
        l = snprintf(f, sizeof f, "42%lu[\"sessionCreate\",null]", (unsigned long)c->enter_ack);
    }
    send(c, f, (size_t)l);
    c->state = CELIO_ENTERING;
    c->deadline = now + ENTER_MS;
}

static void begin_link(celio_t *c, uint32_t now) {
    cancel_adapter(c);
    c->state = CELIO_STARTING;
    c->step = 0;
    c->deadline = now + CANCEL_MS;
    c->tx_seq = 0;
    c->rx_seq = 0;
    memset(c->held, 0, sizeof c->held);
}

static void on_enter_reply(celio_t *c, json_t args, uint32_t now) {
    json_t res, v;
    if (!json_at(args, 0, &res) || !json_get(res, "variant", &v)) {
        fail(c, "bad server reply");
        return;
    }
    if (json_str_is(v, "Ok")) {
        json_t val, id, full;
        char sid[CELIO_SESSION_MAX + 1];
        if (!json_get(res, "value", &val) || !json_get(val, "id", &id) ||
            !json_str(id, sid, sizeof sid)) {
            fail(c, "bad server reply");
            return;
        }
        strcpy(c->session, sid);
        bool is_full = json_get(val, "full", &full) && json_type(full) == JSON_TRUE;
        if (c->joining || is_full) {
            begin_link(c, now);   /* the partner is already in */
        } else {
            c->state = CELIO_WAITING;
        }
        return;
    }
    json_t e;
    char why[sizeof c->error];
    if (json_get(res, "error", &e) && json_str(e, why, sizeof why)) {
        fail(c, why);
    } else {
        fail(c, "session refused");
    }
}

static bool seen_before(celio_t *c, const char *uuid) {
    for (int i = 0; i < CELIO_SEEN_CMDS; i++) {
        if (c->seen[i][0] && strcmp(c->seen[i], uuid) == 0) {
            return true;
        }
    }
    snprintf(c->seen[c->seen_next], sizeof c->seen[0], "%s", uuid);
    c->seen_next = (uint8_t)((c->seen_next + 1) % CELIO_SEEN_CMDS);
    return false;
}

static void on_command(celio_t *c, json_t arg) {
    json_t u, cm;
    char uuid[37];
    int64_t v;
    if (!json_get(arg, "command", &cm) || !json_int(cm, &v) || v < 0 || v > 0xFF) {
        return;
    }
    if (json_get(arg, "uuid", &u) && json_str(u, uuid, sizeof uuid) && seen_before(c, uuid)) {
        c->duplicates++;
        return;
    }
    if (c->state != CELIO_LINKED && c->state != CELIO_STARTING) {
        return;
    }
    const uint8_t cmd = (uint8_t)v;
    command(c, &cmd, 1);
}

static void to_adapter(celio_t *c, const uint8_t data[64]) {
    if (c->io.gbl_data) {
        c->io.gbl_data(c->io.ctx, data);
    }
    c->received++;
}

static void flush_held(celio_t *c) {
    for (;;) {
        int slot = (int)(c->rx_seq % CELIO_REORDER);
        if (!c->held[slot] || c->held_seq[slot] != c->rx_seq) {
            return;
        }
        c->held[slot] = false;
        to_adapter(c, c->held_data[slot]);
        c->rx_seq++;
    }
}

/*
 * In sequence order, as LinkExchangeSession.handleSocketDataToDevice does.
 * Held packets always lie in [rx_seq, rx_seq + CELIO_REORDER), so each has a
 * slot of its own.
 */
static void deliver(celio_t *c, uint32_t seq, const uint8_t data[64]) {
    uint32_t ahead = seq - c->rx_seq;
    if ((int32_t)ahead < 0) {
        c->duplicates++;
        return;
    }
    int slot = (int)(seq % CELIO_REORDER);
    if (ahead && c->held[slot] && c->held_seq[slot] == seq) {
        c->duplicates++;
        return;
    }
    if (ahead >= 2 * CELIO_REORDER) {
        /* Far ahead: everything between is gone. Start again from here. */
        c->dropped += ahead;
        memset(c->held, 0, sizeof c->held);
        c->rx_seq = seq;
        ahead = 0;
    }
    while (ahead >= CELIO_REORDER) {
        /*
         * Too far ahead to hold: the packet at rx_seq is not coming (the
         * server gave up on it). Skip it, and deliver what was held behind
         * it, rather than stall the link for good as Celio-Client would.
         * seq itself is not held, so this stops at or before it.
         */
        c->dropped++;
        c->rx_seq++;
        flush_held(c);
        ahead = seq - c->rx_seq;
    }
    if (ahead == 0) {
        to_adapter(c, data);
        c->rx_seq++;
        flush_held(c);
        return;
    }
    c->held[slot] = true;
    c->held_seq[slot] = seq;
    memcpy(c->held_data[slot], data, 64);
}

static void on_data_packet(celio_t *c, json_t pkt) {
    json_t sq, d, w;
    int64_t seq, v;
    if (!json_get(pkt, "sequence", &sq) || !json_int(sq, &seq) || seq < 0 ||
        seq > 0xFFFFFFFFll || !json_get(pkt, "data", &d)) {
        return;
    }
    uint8_t out[64];
    for (size_t i = 0; i < 32; i++) {
        if (!json_at(d, i, &w) || !json_int(w, &v)) {
            return;   /* short or malformed: not a packet */
        }
        out[i * 2] = (uint8_t)v;          /* words are & 0xFFFF'd by the sender */
        out[i * 2 + 1] = (uint8_t)(v >> 8);
    }
    if (c->state == CELIO_LINKED) {
        deliver(c, (uint32_t)seq, out);
    }
}

static void on_event(celio_t *c, json_t args, uint32_t now) {
    json_t name, arg = { "null", 4 };
    if (!json_at(args, 0, &name) || json_type(name) != JSON_STRING) {
        return;
    }
    json_at(args, 1, &arg);
    if (json_str_is(name, "deviceCommand")) {
        on_command(c, arg);
    } else if (json_str_is(name, "deviceData")) {
        if (json_type(arg) == JSON_ARRAY) {   /* the patched server batches */
            json_t p;
            for (size_t i = 0; json_at(arg, i, &p); i++) {
                on_data_packet(c, p);
            }
        } else {
            on_data_packet(c, arg);
        }
    } else if (json_str_is(name, "partnerJoined")) {
        if (c->state == CELIO_WAITING) {
            begin_link(c, now);
        }
    } else if (json_str_is(name, "partnerLeft")) {
        if (c->state == CELIO_STARTING || c->state == CELIO_LINKED) {
            cancel_adapter(c);
        }
        if (c->state >= CELIO_WAITING && c->state <= CELIO_LINKED) {
            c->state = CELIO_WAITING;
        }
    } else if (json_str_is(name, "sessionClose")) {
        if (c->state == CELIO_STARTING || c->state == CELIO_LINKED) {
            cancel_adapter(c);
        }
        if (c->state != CELIO_FAILED) {
            c->state = CELIO_CLOSED;
        }
    }
}

void celio_on_text(celio_t *c, const char *text, size_t len, uint32_t now) {
    sio_packet_t p;
    if (c->state == CELIO_IDLE || !sio_parse(text, len, &p)) {
        return;
    }
    switch (p.eio) {
    case '0': {
        /* Engine.IO open: join the default namespace with Celio's auth */
        char f[80];
        int l = snprintf(f, sizeof f, "40{\"clientId\":\"%s\"}", c->client_id);
        send(c, f, (size_t)l);
        return;
    }
    case '2':
        send_str(c, "3");   /* pong */
        return;
    case '1':
        celio_on_close(c, now);
        return;
    case '4':
        break;
    default:
        return;
    }
    json_t j = { NULL, 0 };
    bool have = p.json_len && json_parse(p.json, p.json_len, &j);
    switch (p.sio) {
    case '0':
        if (c->lost_since || c->connected) {
            c->reconnects++;
        }
        c->connected = true;
        c->lost_since = 0;
        if (c->state == CELIO_CONNECTING) {
            enter_session(c, now);
        }
        return;
    case '4':
        fail(c, "server refused");
        return;
    case '1':
        celio_on_close(c, now);
        return;
    case '3':
        if (have && p.has_id && p.id == c->enter_ack && c->state == CELIO_ENTERING) {
            on_enter_reply(c, j, now);
        }
        return;
    case '2':
        if (p.has_id) {
            ack(c, p.id);   /* the patched server retries until it hears this */
        }
        if (have && json_type(j) == JSON_ARRAY) {
            on_event(c, j, now);
        }
        return;
    default:
        return;
    }
}

void celio_on_status(celio_t *c, uint16_t status, uint32_t now) {
    (void)now;
    c->link_status = status;
    if (c->state == CELIO_STARTING && c->step == 1 && status == ST_DEVICE_READY) {
        c->state = CELIO_LINKED;
        return;
    }
    if (c->state != CELIO_LINKED) {
        return;   /* Cancel's echoes are not the session's business */
    }
    if (status == ST_DEVICE_READY || status == ST_TRADE_DONE || status == ST_DEBUG) {
        return;   /* LinkExchangeSession.handleDeviceStatusToSocket drops these */
    }
    char uuid[37], f[128];
    make_uuid(c, uuid);
    int l = snprintf(f, sizeof f, "42[\"deviceStatus\",{\"uuid\":\"%s\",\"linkStatus\":%u}]",
                     uuid, (unsigned)status);
    send(c, f, (size_t)l);
}

void celio_on_data(celio_t *c, const uint8_t *data, size_t len, uint32_t now) {
    (void)now;
    if (c->state != CELIO_LINKED || len != 64) {
        return;
    }
    char f[CELIO_FRAME_MAX];
    int l = snprintf(f, sizeof f, "42[\"deviceData\",{\"sequence\":%lu,\"data\":[",
                     (unsigned long)c->tx_seq++);
    for (int i = 0; i < 32; i++) {
        unsigned w = (unsigned)data[i * 2] | (unsigned)data[i * 2 + 1] << 8;
        l += snprintf(f + l, sizeof f - (size_t)l, i ? ",%u" : "%u", w);
    }
    l += snprintf(f + l, sizeof f - (size_t)l, "]}]");
    send(c, f, (size_t)l);
}

void celio_tick(celio_t *c, uint32_t now) {
    switch (c->state) {
    case CELIO_CONNECTING:
        if (due(now, c->deadline)) {
            fail(c, "no server");
        }
        return;
    case CELIO_ENTERING:
        if (due(now, c->deadline)) {
            fail(c, "no reply");
        }
        return;
    case CELIO_STARTING:
        if (!due(now, c->deadline)) {
            break;
        }
        if (c->step == 0) {
            const uint8_t setmode[2] = { CMD_SETMODE, 0x01 };   /* LinkMode.onlineLink */
            command(c, setmode, sizeof setmode);
            c->step = 1;
            c->deadline = now + READY_MS;
        } else {
            fail(c, "adapter did not start");
        }
        break;
    default:
        break;
    }
    if (c->lost_since && c->state >= CELIO_WAITING && c->state <= CELIO_LINKED &&
        due(now, c->lost_since + CELIO_LOST_MS)) {
        fail(c, "server lost");
    }
}

const char *celio_state_name(celio_state_t s) {
    switch (s) {
    case CELIO_IDLE:       return "idle";
    case CELIO_CONNECTING: return "connecting";
    case CELIO_ENTERING:   return "entering";
    case CELIO_WAITING:    return "waiting";
    case CELIO_STARTING:   return "starting";
    case CELIO_LINKED:     return "linked";
    case CELIO_CLOSED:     return "closed";
    case CELIO_FAILED:     return "failed";
    default:               return "?";
    }
}
