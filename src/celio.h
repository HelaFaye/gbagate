/*
 * celio.h -- online play through Celio, from the board itself.
 *
 * Celio (github.com/Celio-Link) relays a Game Boy link between two GB-Link
 * adapters over the internet: Celio-Client, a web page, drives each adapter
 * over WebUSB and talks to Celio-Server over Socket.IO. This is Celio-Client's
 * part, in C, so the board can play online with no PC: the WiFi connection
 * replaces the browser, and GB-Link's local host (Transport's Local route,
 * PATCHES.md P8) replaces WebUSB.
 *
 * It is a state machine with no I/O of its own. The caller feeds it what the
 * WebSocket received, the status words and data packets the local GB-Link
 * firmware reported, and the time; it calls back to send a WebSocket text
 * frame or to hand the firmware a command or a data packet. So the same code
 * runs on the board and in the simulator, where test/gblink_compat drives it
 * against the real Celio-Server.
 *
 * What it speaks is the Socket.IO protocol of Celio-Client before b057315,
 * the one the public Celio-Server (9f02d3e) serves: status and data emitted
 * without acknowledgements. It acknowledges whatever the server sends with an
 * ack id, so it works with backport/Celio-Server's patched server too, which
 * serves both kinds of client.
 *
 * The flow, as Celio-Client's onlineLink page runs it:
 *   connect, then create a session (and show its id) or join one;
 *   once both players are in, Cancel, 500 ms, SetMode 0x01, await DeviceReady;
 *   then relay: the adapter's status words up as deviceStatus, its 64-byte
 *   data packets up as deviceData with a sequence number; the server's
 *   deviceCommand down as a one-byte command, its deviceData down in order.
 */

#ifndef CELIO_H
#define CELIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELIO_SESSION_MAX 32
#define CELIO_REORDER     16    /* data packets held while one is missing   */
#define CELIO_SEEN_CMDS   16    /* command uuids remembered, for duplicates */
#define CELIO_FRAME_MAX   512   /* the longest frame it sends               */

typedef struct {
    /* One WebSocket text frame to the server. False if it could not be sent
     * (not connected, buffer full); the frame is then lost, as socket.io
     * would buffer and the server would time out. */
    bool (*ws_send)(void *ctx, const char *text, size_t len);
    /* A command to the local GB-Link firmware, [command, args...]. */
    void (*gbl_command)(void *ctx, const uint8_t *cmd, size_t len);
    /* A 64-byte data packet to it. */
    void (*gbl_data)(void *ctx, const uint8_t data[64]);
    /* 32 random bits, for uuids. */
    uint32_t (*random)(void *ctx);
    void *ctx;
} celio_io_t;

typedef enum {
    CELIO_IDLE = 0,
    CELIO_CONNECTING,     /* waiting for the server's Socket.IO handshake */
    CELIO_ENTERING,       /* sessionCreate / sessionJoin sent             */
    CELIO_WAITING,        /* in a session, alone                          */
    CELIO_STARTING,       /* partner here: Cancel, SetMode, DeviceReady   */
    CELIO_LINKED,         /* relaying                                     */
    CELIO_CLOSED,         /* the server ended the session                 */
    CELIO_FAILED,         /* see celio_t.error                            */
} celio_state_t;

typedef struct {
    celio_io_t    io;
    celio_state_t state;
    bool          joining;                     /* join `session`, not create */
    bool          connected;                   /* Socket.IO namespace open   */
    char          client_id[37];               /* auth.clientId, per start   */
    char          session[CELIO_SESSION_MAX + 1];
    char          error[40];                   /* why FAILED, for the screen */

    uint32_t      next_ack;                    /* our emits' ack ids          */
    uint32_t      enter_ack;                   /* the create/join awaiting its reply */
    uint32_t      deadline;                    /* the current step's timeout  */
    uint32_t      lost_since;                  /* when the socket dropped, 0 if up */
    uint8_t       step;                        /* within STARTING             */

    uint16_t      link_status;                 /* the adapter's last word     */
    uint32_t      tx_seq;                      /* our next data sequence      */
    uint32_t      rx_seq;                      /* the next one we expect      */
    bool          held[CELIO_REORDER];
    uint32_t      held_seq[CELIO_REORDER];
    uint8_t       held_data[CELIO_REORDER][64];

    char          seen[CELIO_SEEN_CMDS][37];
    uint8_t       seen_next;

    /* counters, for the screen and for tests */
    uint32_t      sent, received, dropped, duplicates, reconnects;
} celio_t;

/*
 * Start: create a session (session NULL or empty) or join one. Expects
 * celio_on_open() next, once the caller has the WebSocket connected to
 * <server>/socket.io/?EIO=4&transport=websocket.
 */
void celio_start(celio_t *c, const celio_io_t *io, const char *session, uint32_t now_ms);

/* Leave: sessionLeft if in one, Cancel to the adapter. The state is IDLE. */
void celio_stop(celio_t *c);

/* The WebSocket opened (or reopened: a reconnect keeps the session, as the
 * server keeps a client's place for five seconds by its clientId). */
void celio_on_open(celio_t *c, uint32_t now_ms);
/* It closed. Reopen it; after CELIO_LOST_MS the session is given up. */
void celio_on_close(celio_t *c, uint32_t now_ms);
#define CELIO_LOST_MS 10000

/* A text frame from the server. */
void celio_on_text(celio_t *c, const char *text, size_t len, uint32_t now_ms);

/* What the local firmware reported, from Transport's local sink. */
void celio_on_status(celio_t *c, uint16_t status, uint32_t now_ms);
void celio_on_data(celio_t *c, const uint8_t *data, size_t len, uint32_t now_ms);

/* Timeouts and the Cancel-to-SetMode pause. Call every few milliseconds. */
void celio_tick(celio_t *c, uint32_t now_ms);

const char *celio_state_name(celio_state_t s);

/* ---- the Socket.IO framing, exposed for its tests ------------------------ */

typedef struct {
    char        eio;        /* Engine.IO type: '0' open, '2' ping, '4' message... */
    char        sio;        /* for '4': Socket.IO type, '0' connect, '2' event, '3' ack */
    bool        has_id;
    uint32_t    id;         /* the ack id, when has_id                     */
    const char *json;       /* the payload after the header, may be empty  */
    size_t      json_len;
} sio_packet_t;

/* Split a text frame. False if it is not one this client understands
 * (another namespace, a binary placeholder). */
bool sio_parse(const char *text, size_t len, sio_packet_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CELIO_H */
