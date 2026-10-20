/*
 * gbx_proto.h -- gbagate's extensions to the GB-Link host protocol ("GBX").
 *
 * A gbagate board is a GB-Link adapter first: every upstream command, mode and
 * status means here exactly what it means on a GB-Link. On top of that it
 * answers a few things a GB-Link does not have, chosen so that a GB-Link
 * ignores them and an upstream host never sends them:
 *
 *   0x4F  GET_EXTENDED_INFO   a hardware-range (0x4x) command, so it is
 *                             dispatched in every mode. A GB-Link has no 0x4F
 *                             and sends nothing back, which is how a host
 *                             tells the two apart (time out = GB-Link).
 *   0x80+ modes               above every upstream mode number (0x00-0x07 so
 *                             far), leaving the low range to upstream.
 *   0x5x  mode commands       the same per-mode range scheme upstream uses
 *                             (0x1x link, 0x2x trade, 0x3x GB); 0x5x is free.
 *
 * Replies travel like upstream's: command id echoed in byte 0, on the data
 * endpoint (or data channel on serial). Little-endian throughout.
 *
 * If GB-Link ever assigns any of these numbers, upstream wins and this moves.
 */
#ifndef GBX_PROTO_H
#define GBX_PROTO_H

#include <stdint.h>

#define GBAGATE_VERSION_MAJOR 0
#define GBAGATE_VERSION_MINOR 4
#define GBAGATE_VERSION_PATCH 0

#define GBX_EXT_VERSION 1

/* ---- 0x4F GET_EXTENDED_INFO ------------------------------------------------
 *
 * Reply, 16 bytes:
 *   [0]     0x4F
 *   [1..3]  'G' 'B' 'X'
 *   [4]     GBX_EXT_VERSION
 *   [5..7]  gbagate firmware major, minor, patch
 *   [8..11] capability bits, u32 LE (GBX_CAP_*)
 *   [12]    board (GBX_BOARD_*)
 *   [13]    image (variant_id_t: 0 link, 1 gate, 2 full, 3 mini)
 *   [14..15] reserved, 0
 */
#define GBX_CMD_GET_EXTENDED_INFO 0x4Fu
#define GBX_INFO_LEN 16u

#define GBX_CAP_CHIPGATE    (1u << 0)  /* mode 0x80                          */
#define GBX_CAP_SD_ALT      (1u << 1)  /* second SD path fitted (GBC cable)  */
#define GBX_CAP_RGB_LED     (1u << 2)
#define GBX_CAP_VCC_SWITCH  (1u << 3)  /* 0x40/0x41 really switch VCC        */
#define GBX_CAP_DISPLAY     (1u << 4)
#define GBX_CAP_STORAGE     (1u << 5)  /* programs on SD or USB drive        */
#define GBX_CAP_ESPNOW      (1u << 6)  /* board-to-board radio               */
#define GBX_CAP_MULTIBOOT   (1u << 7)  /* standalone Send Program            */
#define GBX_CAP_BRIDGE      (1u << 8)  /* GB-Link's bridge UART is wired      */

#define GBX_BOARD_HOST      0u         /* the host simulator                 */
#define GBX_BOARD_DEVKIT    1u
#define GBX_BOARD_SUPERMINI 2u

/* ---- mode 0x80: Battle Chip Gate --------------------------------------------
 *
 *   [0x00, 0x80, game]   game = cg_game_t + 1 (1 BN4, 2 BN5, 3 EXE6 JP,
 *                        4 MMBN6 US/EU); 0 means MMBN6 US/EU.
 *
 * The adapter is the gate: a child on the bus, answering the game. Statuses:
 *   FF08 DeviceReady   as for every mode
 *   FF02 AwaitMode     gate armed, waiting for the game's 0x8FFF
 *   FF05 LinkConnected the game opened a gate session
 *   FF06 LinkReconnecting the session closed or the game stopped clocking
 *   FF07 LinkClosed    mode left
 *
 * Mode commands:
 *   0x50 INSERT_CHIP  [idLo, idHi]  reply [0x50, accepted]
 *                     (refused while a chip is still being sent, like a real
 *                     gate holding one chip)
 *   0x51 REMOVE_CHIP                reply [0x51, 1]
 *   0x52 GET_STATE                  reply [0x52, link state, slot, chipLo,
 *                                   chipHi, game, sessions u32, frames u32]
 *   0x53 SET_GAME     [game]        reply [0x53, game]
 *
 * Event, unprompted on the data endpoint:
 *   [0x54, idLo, idHi]  CHIP_SENT: the chip finished its nine rotations
 */
#define GBX_MODE_CHIPGATE    0x80u
#define GBX_CMD_INSERT_CHIP  0x50u
#define GBX_CMD_REMOVE_CHIP  0x51u
#define GBX_CMD_GET_STATE    0x52u
#define GBX_CMD_SET_GAME     0x53u
#define GBX_EVT_CHIP_SENT    0x54u
#define GBX_STATE_LEN        15u

#endif /* GBX_PROTO_H */
