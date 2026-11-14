/*
 * mmo_relay.c -- see mmo_relay.h.
 */

#include "mmo_relay.h"

#include <stdlib.h>
#include <string.h>

/* MOVEMENT_ACTION_*: pokeemerald include/constants/event_object_movement.h */
#define FACE_DOWN  0x00
#define WALK_DOWN  0x08
#define DIR_SOUTH  1
#define DIR_NORTH  2
#define DIR_WEST   3
#define DIR_EAST   4

/* MOVEMENT_ACTION_FACE_* and _WALK_NORMAL_* run down, up, left, right. */
static uint16_t face(uint8_t dir) {
    return (uint16_t)(FACE_DOWN + (dir >= DIR_SOUTH && dir <= DIR_EAST ? dir - 1 : 0));
}

static uint8_t walk_dir(int dx, int dy) {
    return dx > 0 ? DIR_EAST : dx < 0 ? DIR_WEST : dy > 0 ? DIR_SOUTH : DIR_NORTH;
}

static uint16_t walk(uint8_t dir) {
    return (uint16_t)(WALK_DOWN + dir - 1);
}

static bool same_map(const mmo_state_t *a, const mmo_state_t *b) {
    return a->group == b->group && a->num == b->num;
}

static uint16_t map_word(const mmo_state_t *s) {
    return (uint16_t)(s->group << 8 | s->num);
}

/* ---- the queue to the agent --------------------------------------------- */

static bool push(mmo_relay_t *r, uint8_t type, int n, const uint16_t *w) {
    if (r->qn >= MMO_QUEUE) {
        r->overflows++;
        return false;
    }
    mmo_msg_t *m = &r->q[(r->qh + r->qn) % MMO_QUEUE];
    m->type = type;
    m->n = (uint8_t)n;
    memcpy(m->w, w, (size_t)n * sizeof *w);
    r->qn++;
    return true;
}

static int room(const mmo_relay_t *r) {
    return MMO_QUEUE - r->qn;
}

static bool spawn(mmo_relay_t *r, int p) {
    const mmo_state_t *s = &r->peer[p].st;
    uint16_t w[6] = {
        (uint16_t)(MMO_LOCALID + p), s->gender ? MMO_GFX_MAY : MMO_GFX_BRENDAN,
        (uint16_t)s->x, (uint16_t)s->y, s->elevation, map_word(s),
    };
    uint16_t f[2] = { (uint16_t)(MMO_LOCALID + p), face(s->facing) };
    if (room(r) < 2) {
        return false;
    }
    push(r, MMO_SPAWN, 6, w);
    push(r, MMO_MOVE, 2, f);
    r->peer[p].shown = true;
    r->peer[p].shown_st = *s;
    return true;
}

static bool remove_fig(mmo_relay_t *r, int p) {
    uint16_t w[1] = { (uint16_t)(MMO_LOCALID + p) };
    if (!push(r, MMO_REMOVE, 1, w)) {
        return false;
    }
    r->peer[p].shown = false;
    return true;
}

static bool move(mmo_relay_t *r, int p, uint16_t action) {
    uint16_t w[2] = { (uint16_t)(MMO_LOCALID + p), action };
    return push(r, MMO_MOVE, 2, w);
}

/* Bring player p's figure to where the server last saw them. */
static void show(mmo_relay_t *r, int p) {
    mmo_peer_t *q = &r->peer[p];
    if (!r->have_me || !q->known) {
        return;
    }
    if (!same_map(&q->st, &r->me)) {
        if (q->shown && !remove_fig(r, p)) {
            q->resync = true;
            return;
        }
        q->resync = false;
        if (q->st.group == 0xFF && q->st.num == 0xFF) {
            q->known = false;                  /* gone, and its figure with it */
        }
        return;
    }
    if (q->resync) {                       /* a step was lost: place it again */
        if (room(r) < 3) {
            return;
        }
        if (q->shown) {
            remove_fig(r, p);
        }
        spawn(r, p);
        q->resync = false;
        return;
    }
    if (!q->shown) {
        if (!spawn(r, p)) {
            q->resync = true;
        }
        return;
    }
    int dx = q->st.x - q->shown_st.x, dy = q->st.y - q->shown_st.y;
    if (dx == 0 && dy == 0) {
        if (q->st.facing != q->shown_st.facing) {
            if (move(r, p, face(q->st.facing))) {
                q->shown_st.facing = q->st.facing;
            } else {
                q->resync = true;
            }
        }
        return;
    }
    if (abs(dx) + abs(dy) == 1) {          /* one step */
        uint8_t d = walk_dir(dx, dy);
        if (!move(r, p, walk(d))) {
            q->resync = true;
            return;
        }
        q->shown_st.x = q->st.x;
        q->shown_st.y = q->st.y;
        q->shown_st.facing = d;
        if (q->st.facing != d && move(r, p, face(q->st.facing))) {
            q->shown_st.facing = q->st.facing;
        }
        return;
    }
    /* More than a step (a stall, a warp within the map): place it. */
    if (room(r) < 3) {
        q->resync = true;
        return;
    }
    remove_fig(r, p);
    spawn(r, p);
}

/* ---- the agent's side --------------------------------------------------- */

static void own_state(mmo_relay_t *r, const mmo_msg_t *m) {
    if (m->n < 6) {
        return;
    }
    mmo_state_t s = {
        .group = (uint8_t)(m->w[0] >> 8), .num = (uint8_t)m->w[0],
        .x = (int16_t)m->w[1], .y = (int16_t)m->w[2],
        .facing = (uint8_t)(m->w[3] & 0x0F), .elevation = (uint8_t)((m->w[3] >> 4) & 0x0F),
        .gender = (uint8_t)((m->w[3] >> 8) & 1),
    };
    r->acked = m->w[4] & 0x7FFF;
    r->free = (uint8_t)m->w[5];
    r->states++;

    bool first = !r->agent_seen;
    r->agent_seen = true;
    bool moved_map = r->have_me && !same_map(&s, &r->me);
    bool changed = !r->have_me || memcmp(&s, &r->me, sizeof s) != 0;
    r->me = s;
    r->have_me = true;

    if (first || moved_map) {
        /* A new map (or an agent we have not heard from): the game holds no
         * figures. What was queued was for the map before, and the agent has
         * emptied its mailbox: count from where it is. A frame half sent is
         * dropped by its reader. */
        r->qn = 0;
        r->qh = 0;
        r->out_n = r->out_i = 0;
        r->sent = r->acked;
        for (int p = 0; p < MMO_MAX_PLAYERS; p++) {
            r->peer[p].shown = false;
            r->peer[p].resync = false;
        }
        for (int p = 0; p < MMO_MAX_PLAYERS; p++) {
            if (p != r->pid) {
                show(r, p);
            }
        }
    }
    if (changed && r->send) {
        mn_msg_t out = { .type = MN_STATE, .st = s };
        r->send(r->ctx, &out);
    }
}

/* The next word to the agent: the frame going out, else the next command it
 * has room for, else idle. */
static uint16_t next_word(mmo_relay_t *r) {
    if (r->out_i < r->out_n) {
        return r->out[r->out_i++];
    }
    for (int p = 0; p < MMO_MAX_PLAYERS && r->qn < MMO_QUEUE - 2; p++) {
        if (r->peer[p].resync && p != r->pid) {
            show(r, p);
        }
    }
    uint16_t in_flight = (uint16_t)((r->sent - r->acked) & 0x7FFF);
    if (in_flight > 2 * MMO_QUEUE) {           /* the agent's count went back */
        r->sent = r->acked;
        in_flight = 0;
    }
    if (r->qn > 0 && r->agent_seen && in_flight < r->free) {
        mmo_msg_t *m = &r->q[r->qh];
        r->out_n = mmo_encode(m, r->out, (int)(sizeof r->out / sizeof r->out[0]));
        r->out_i = 0;
        r->qh = (r->qh + 1) % MMO_QUEUE;
        r->qn--;
        r->sent = (uint16_t)((r->sent + 1) & 0x7FFF);
        r->commands++;
        if (r->out_n > 0) {
            return r->out[r->out_i++];
        }
    }
    return MMO_IDLE;
}

/* ---- the API ------------------------------------------------------------ */

void mmo_relay_init(mmo_relay_t *r, mmo_send_fn send, void *ctx) {
    memset(r, 0, sizeof *r);
    r->send = send;
    r->ctx = ctx;
    r->pid = -1;
    mmo_rx_init(&r->rx);
    if (send) {
        mn_msg_t hello = { .type = MN_HELLO, .version = MMO_NET_VERSION };
        send(ctx, &hello);
    }
}

uint16_t mmo_relay_word(mmo_relay_t *r, uint16_t from_game) {
    r->words++;
    if (mmo_rx_word(&r->rx, from_game) && r->rx.msg.type == MMO_STATE) {
        own_state(r, &r->rx.msg);
    }
    return next_word(r);
}

void mmo_relay_net(mmo_relay_t *r, const mn_msg_t *m) {
    switch (m->type) {
    case MN_WELCOME:
        r->pid = m->pid < MMO_MAX_PLAYERS ? m->pid : -1;
        if (r->have_me && r->send) {       /* tell the server where we are */
            mn_msg_t out = { .type = MN_STATE, .st = r->me };
            r->send(r->ctx, &out);
        }
        break;
    case MN_PLAYER:
        if (m->pid < MMO_MAX_PLAYERS && m->pid != r->pid) {
            r->peer[m->pid].known = true;
            r->peer[m->pid].st = m->st;
            show(r, m->pid);
        }
        break;
    case MN_GONE:
        if (m->pid < MMO_MAX_PLAYERS && m->pid != r->pid) {
            /* On no map: show() takes the figure away, now or when there
             * is room, and forgets them once it has. */
            mmo_peer_t *q = &r->peer[m->pid];
            q->st.group = q->st.num = 0xFF;
            show(r, m->pid);
            if (!q->shown && !q->resync) {
                q->known = false;
            }
        }
        break;
    default:
        break;
    }
}

int mmo_relay_pending(const mmo_relay_t *r) {
    return r->qn + (r->out_i < r->out_n ? 1 : 0);
}
