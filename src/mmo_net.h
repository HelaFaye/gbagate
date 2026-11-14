/*
 * mmo_net.h -- the shared overworld's messages between the board and the
 * server (doc/MMO.md, "Netcode").
 *
 * Over a byte stream (a UART under emulation; on a board, its network
 * connection), each message SLIP-framed:
 *
 *     END  type  body...  crc16 (CCITT-FALSE over type and body, LE)  END
 *
 * Board -> server
 *   MN_HELLO    version
 *   MN_STATE    the board's player: mmo_state_t
 * Server -> board
 *   MN_WELCOME  the player id the server gave this board, and the most there
 *               can be
 *   MN_PLAYER   another player, where the server last saw them: pid, state.
 *               Sent when it changes, for players on the same map.
 *   MN_GONE     pid: no longer on this board's map, or no longer playing
 */

#ifndef MMO_NET_H
#define MMO_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MMO_NET_VERSION 1
#define MMO_MAX_PLAYERS 16

typedef enum {
    MN_HELLO   = 0x01,
    MN_STATE   = 0x02,
    MN_WELCOME = 0x81,
    MN_PLAYER  = 0x82,
    MN_GONE    = 0x83,
} mn_type_t;

/* Where a player is: what MMO_STATE carries, as the server sees it. */
typedef struct {
    uint8_t group, num;     /* map                                   */
    int16_t x, y;           /* tile                                   */
    uint8_t facing;         /* DIR_SOUTH 1, NORTH 2, WEST 3, EAST 4   */
    uint8_t elevation;
    uint8_t gender;         /* 0 male (Brendan), 1 female (May)       */
} mmo_state_t;

typedef struct {
    uint8_t     type;
    uint8_t     pid;        /* MN_WELCOME, MN_PLAYER, MN_GONE         */
    uint8_t     max;        /* MN_WELCOME                             */
    uint8_t     version;    /* MN_HELLO                               */
    mmo_state_t st;         /* MN_STATE, MN_PLAYER                    */
} mn_msg_t;

#define MN_MAX_FRAMED 48

/* One message SLIP-framed into out. Bytes written, 0 if it does not fit. */
size_t mn_encode(const mn_msg_t *m, uint8_t *out, size_t cap);

typedef struct {
    uint8_t  buf[24];
    size_t   len;
    bool     esc;
    bool     overflow;
    uint32_t bad;           /* frames dropped: CRC, length, overflow */
} mn_rx_t;

void mn_rx_init(mn_rx_t *r);

/* One received byte. True when it ended a good frame, now in *out. */
bool mn_rx_byte(mn_rx_t *r, uint8_t byte, mn_msg_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MMO_NET_H */
