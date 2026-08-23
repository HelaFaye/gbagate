/*
 * link_proto.h -- wire protocol between the ESP32 link adapter and a host
 *                 (mGBA over USB CDC, or a peer adapter over ESP-NOW/UDP).
 *
 * One transport-agnostic packet format, two framings:
 *   - byte streams (USB CDC, UART): SLIP framed, use lp_encode / lp_rx_feed.
 *   - datagrams   (ESP-NOW, UDP):   use lp_encode_raw / lp_decode_raw, since
 *                                   the transport already delimits messages.
 *
 * Packet layout before framing:
 *
 *     [type:1][seq:2 LE][payload:0..16][crc16:2 LE]
 *
 * CRC is CCITT-FALSE over type..payload. It earns its keep on ESP-NOW, where
 * a corrupt frame is otherwise indistinguishable from a real one.
 */

#ifndef LINK_PROTO_H
#define LINK_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LP_VERSION      1
#define LP_MAX_PACKET   24  /* unframed */
#define LP_MAX_FRAMED   (LP_MAX_PACKET * 2 + 2)

/* 0xFFFF is reserved on the GBA bus: it is what every reader sees for a slot
 * nobody drove. Never relay it as if it were real payload. */
#define GBA_WORD_EMPTY  0xFFFFu

typedef enum {
    GBA_ROLE_SLAVE  = 0, /* child: the local GBA clocks us                   */
    GBA_ROLE_MASTER = 1  /* parent: we clock the local GBA, so we set the pace*/
} gba_role_t;

/* The result of one multiplayer transfer, as every participant sees it. */
typedef struct {
    uint16_t word[4];  /* SIOMULTI0..3; GBA_WORD_EMPTY for absent players */
    uint8_t  players;  /* how many slots were actually driven, 1..4       */
    uint8_t  self_id;  /* our own multiplayer ID, 0..3                    */
} gba_transfer_t;

typedef enum {
    LP_HELLO     = 0x01, /* both ways: announce version, role, player count */
    LP_HELLO_ACK = 0x02,
    LP_WORD      = 0x10, /* host -> adapter: my word for this sequence      */
    LP_RESULT    = 0x11, /* adapter -> host: a transfer completed           */
    LP_UNDERRUN  = 0x12, /* adapter -> host: host was late, sent 0xFFFF     */
    LP_RESET     = 0x20, /* either way: drop the session, resync sequence   */
    LP_PING      = 0x30,
    LP_PONG      = 0x31
} lp_type_t;

typedef struct {
    lp_type_t type;
    uint16_t  seq;
    union {
        struct { uint8_t version; uint8_t role; uint8_t players; } hello;
        struct { uint16_t word; } word;
        gba_transfer_t result;
        struct { uint32_t stamp_us; } ping;
    } u;
} lp_packet_t;

/* ---- datagram framing ------------------------------------------------ */

/* Returns bytes written, or 0 if cap is too small / the packet is malformed. */
size_t lp_encode_raw(const lp_packet_t *pkt, uint8_t *out, size_t cap);

/* Returns true if the buffer holds one valid, CRC-clean packet. */
bool lp_decode_raw(const uint8_t *in, size_t len, lp_packet_t *out);

/* ---- byte-stream framing (SLIP) -------------------------------------- */

size_t lp_encode(const lp_packet_t *pkt, uint8_t *out, size_t cap);

typedef struct {
    uint8_t buf[LP_MAX_PACKET];
    size_t  len;
    bool    escaped;
    bool    overflow;   /* a frame ran long; it will be dropped at END */
    uint32_t dropped;   /* frames lost to overflow or bad CRC          */
} lp_rx_t;

void lp_rx_init(lp_rx_t *rx);

/* Feed one received byte. Returns true when *out holds a complete packet. */
bool lp_rx_feed(lp_rx_t *rx, uint8_t byte, lp_packet_t *out);

const char *lp_type_name(lp_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* LINK_PROTO_H */
