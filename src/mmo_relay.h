/*
 * mmo_relay.h -- the board's part of the shared overworld (doc/MMO.md).
 *
 * Between the in-game agent (link words, mmo_link.h) and the server
 * (messages, mmo_net.h):
 *
 *   - the agent's MMO_STATE becomes MN_STATE to the server, when it changes;
 *   - the server's MN_PLAYER and MN_GONE become the agent's commands, as the
 *     overworld test's server stand-in made them: a figure is spawned when a
 *     player arrives on this map (then turned their way), walked one step at
 *     a time, turned in place, placed again after a stall or a warp, and
 *     removed when they leave;
 *   - commands go to the agent no faster than its mailbox has room for: its
 *     MMO_STATE says how many it has received and how many slots are free.
 *
 * When this board's own player changes map, the game has dropped every
 * figure: the queue is emptied and the players on the new map are spawned
 * afresh. A command queue that overflows (an agent that stopped taking them)
 * marks the players it lost a step for, and they are placed again once
 * there is room.
 *
 * Not thread-safe: the firmware calls it from the link task and its network
 * task under one lock.
 */

#ifndef MMO_RELAY_H
#define MMO_RELAY_H

#include <stdbool.h>
#include <stdint.h>

#include "mmo_link.h"
#include "mmo_net.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MMO_LOCALID     0x40     /* remote player p is the figure with local id 0x40 + p */
#define MMO_GFX_BRENDAN 0        /* OBJ_EVENT_GFX_BRENDAN_NORMAL */
#define MMO_GFX_MAY     89       /* OBJ_EVENT_GFX_MAY_NORMAL     */
#define MMO_QUEUE       32

typedef void (*mmo_send_fn)(void *ctx, const mn_msg_t *m);

typedef struct {
    bool        known;           /* the server has told us where they are */
    mmo_state_t st;
    bool        shown;           /* a figure is up (or on its way)         */
    mmo_state_t shown_st;        /* where that figure is going to be       */
    bool        resync;          /* lost a step to a full queue            */
} mmo_peer_t;

typedef struct {
    mmo_send_fn send;
    void       *ctx;

    bool        have_me;
    mmo_state_t me;
    int         pid;             /* ours, -1 until MN_WELCOME */

    mmo_peer_t  peer[MMO_MAX_PLAYERS];

    mmo_msg_t   q[MMO_QUEUE];    /* commands for the agent, in order */
    int         qh, qn;

    uint16_t    out[MMO_MAX_WORDS + 2];
    int         out_n, out_i;    /* the frame going out, word by word */

    mmo_rx_t    rx;
    uint16_t    sent;            /* commands sent, mod 0x8000         */
    uint16_t    acked;           /* the agent's count of them         */
    uint8_t     free;            /* its mailbox slots free, last said */
    bool        agent_seen;

    uint32_t    words, states, commands, overflows;
} mmo_relay_t;

/* Says MN_HELLO through send. */
void mmo_relay_init(mmo_relay_t *r, mmo_send_fn send, void *ctx);

/* One link transfer: the word the agent sent, and the word to answer with. */
uint16_t mmo_relay_word(mmo_relay_t *r, uint16_t from_game);

/* A message from the server. */
void mmo_relay_net(mmo_relay_t *r, const mn_msg_t *m);

/* Commands queued and not yet sent: for the screen and the tests. */
int mmo_relay_pending(const mmo_relay_t *r);

#ifdef __cplusplus
}
#endif

#endif /* MMO_RELAY_H */
