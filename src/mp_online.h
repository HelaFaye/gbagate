/*
 * mp_online.h -- a Multi-Play link over Celio: the board's GBAs (one to
 * three, on the carrier's chain) and the other boards' through the server.
 *
 * Between the Celio client in its multi mode (src/celio.h), the four-player
 * relay (src/mp_relay.h, through src/mp_link.h) and the link driver's
 * four-player mode (port/esp32s3/gba_link_mp4.h, src/mp_bus.h):
 *
 *   server command          here
 *   SetModeSlave, ID 0      our port 1 GBA is the parent: the bus as child,
 *                           our GBAs slots 0..gbas-1
 *   SetModeMaster, ID k     a remote player is the parent: the bus as
 *                           parent, our GBAs slots k..k+gbas-1
 *   StartHandshake          the relay answers B9A0 for the remote players
 *   ConnectLink             the remote parent connected: 8FFF in slot 0
 *   Cancel                  the bus stopped
 *
 *   what we report          when
 *   HandshakeReceived       every local GBA is handshaking
 *   LinkConnected           our parent GBA sent 8FFF (the server passes
 *                           ConnectLink to the others)
 *
 * The data between boards is this format, in Celio's 64-byte packets:
 *   [0] 'M'   [1] n (1..3)   then n entries of 17 bytes: the slot, and the
 *   8 command words of that GBA's frame, little-endian. The rest is zero.
 * A packet that is not exactly that, or names a slot that is ours or past
 * the link, is dropped and counted: nothing malformed reaches a GBA.
 */
#ifndef MP_ONLINE_H
#define MP_ONLINE_H

#include <stdbool.h>
#include <stdint.h>

#include "mp_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MPO_MAGIC   'M'
#define MPO_PER_PKT 3

/* Celio's link statuses and commands, as Celio-Server numbers them. */
#define MPO_ST_HANDSHAKE_RECEIVED 0xFF03
#define MPO_ST_LINK_CONNECTED     0xFF05
#define MPO_CMD_CANCEL            0x01
#define MPO_CMD_SET_MASTER        0x10
#define MPO_CMD_SET_SLAVE         0x11
#define MPO_CMD_START_HANDSHAKE   0x12
#define MPO_CMD_CONNECT_LINK      0x13

typedef struct {
    /* Take the link port in this layout, with `link` as the driver's client
     * (mp_link_next / mp_link_done); and give it back. */
    bool (*bus_start)(void *ctx, const mp_bus_layout_t *l, mp_link_t *link);
    void (*bus_stop)(void *ctx);
    /* To the Celio client: celio_on_status and celio_on_data. */
    void (*status)(void *ctx, uint16_t status);
    void (*data_out)(void *ctx, const uint8_t data[64]);
    /* The relay's lock, shared with the link task (NULL: one task). */
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *ctx;
} mp_online_io_t;

typedef struct {
    mp_online_io_t io;
    mp_link_t      link;
    uint8_t        players, gbas;
    int            first;          /* our GBAs' first slot, -1 before a role */
    bool           running;
    bool           said_handshake, said_connected;
    uint32_t       packets_out, commands_in, bad_packets;
} mp_online_t;

void mp_online_init(mp_online_t *m, const mp_online_io_t *io, uint8_t players, uint8_t gbas);

/* The Celio client's mp_command and mp_data (its multi mode). players is
 * the session's, as the client learnt it (celio_t.players). */
void mp_online_command(mp_online_t *m, uint8_t players, uint8_t command, int link_id);
void mp_online_data(mp_online_t *m, uint8_t from, const uint8_t data[64]);

/* Report what changed and send what the local GBAs said. Every few ms. */
void mp_online_tick(mp_online_t *m);

#ifdef __cplusplus
}
#endif

#endif
