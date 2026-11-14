/*
 * test_mmo.c -- the shared overworld's board side: the link words
 * (src/mmo_link.c), the server messages (src/mmo_net.c) and the relay
 * between them (src/mmo_relay.c), against a model of the in-game agent.
 *
 * The model keeps the agent's contract (test/mgba/overworld_sync.py's agent,
 * and the one the adapter tests run in Emerald): a mailbox of eight commands
 * run in order; a walk keeps its figure busy for 16 frames and holds back
 * what follows it; a spawn is for one map and ignored on any other; it
 * reports its player, the commands it has received and the room it has.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmo_link.h"
#include "mmo_net.h"
#include "mmo_relay.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-56s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* ---- the link words ------------------------------------------------------ */

static void test_link(void) {
    printf("link words\n");
    mmo_msg_t m = { .type = MMO_SPAWN, .n = 6, .w = { 0x40, 89, 10, 13, 3, 0x0009 } };
    uint16_t w[20];
    int n = mmo_encode(&m, w, 20);
    check("a spawn is header, six words, check", n, 8);
    check("header", w[0], 0xA500 | MMO_SPAWN << 4 | 6);

    mmo_rx_t rx;
    mmo_rx_init(&rx);
    int got = 0;
    for (int i = 0; i < 3; i++) got += mmo_rx_word(&rx, MMO_IDLE);
    for (int i = 0; i < n; i++) got += mmo_rx_word(&rx, w[i]);
    check("one frame, after idle", got, 1);
    check("its type", rx.msg.type, MMO_SPAWN);
    check("its last word", rx.msg.w[5], 9);

    mmo_msg_t bad = { .type = MMO_MOVE, .n = 2, .w = { 0x40, 0xFFFF } };
    check("0xFFFF is never sent", mmo_encode(&bad, w, 20), 0);

    /* A corrupt word: dropped, and the next frame still read. */
    n = mmo_encode(&m, w, 20);
    w[3] ^= 1;
    got = 0;
    for (int i = 0; i < n; i++) got += mmo_rx_word(&rx, w[i]);
    check("a corrupt frame is dropped", got, 0);
    check("and counted", rx.bad >= 1, 1);
    w[3] ^= 1;
    for (int i = 0; i < n; i++) got += mmo_rx_word(&rx, w[i]);
    check("the next one is read", got, 1);

    /* A lost word: the frame shifts; the reader finds the next header. */
    uint16_t two[40];
    int a = mmo_encode(&m, two, 40);
    int b = mmo_encode(&m, two + a, 40 - a);
    got = 0;
    for (int i = 0; i < a + b; i++) {
        if (i == 2) continue;                       /* lost */
        got += mmo_rx_word(&rx, two[i]);
    }
    check("after a lost word, the next frame", got, 1);

    /* A partner gone mid-frame (0xFFFF) ends it. */
    mmo_rx_init(&rx);
    mmo_rx_word(&rx, two[0]);
    mmo_rx_word(&rx, 0xFFFF);
    got = 0;
    for (int i = 0; i < a; i++) got += mmo_rx_word(&rx, two[i]);
    check("an empty slot ends a frame; the next is read", got, 1);
}

/* ---- the server messages ------------------------------------------------- */

static void test_net(void) {
    printf("server messages\n");
    mn_msg_t m = { .type = MN_PLAYER, .pid = 3,
                   .st = { .group = 0, .num = 9, .x = 0xC0, .y = 0xDB, .facing = 4, .elevation = 3, .gender = 1 } };
    uint8_t buf[MN_MAX_FRAMED];
    size_t n = mn_encode(&m, buf, sizeof buf);
    check("encoded", n > 0, 1);
    mn_rx_t rx;
    mn_rx_init(&rx);
    mn_msg_t out;
    int got = 0;
    for (size_t i = 0; i < n; i++) got += mn_rx_byte(&rx, buf[i], &out);
    check("one message (SLIP's own bytes in x and y)", got, 1);
    check("pid", out.pid, 3);
    check("x", out.st.x, 0xC0);
    check("y", out.st.y, 0xDB);
    check("facing", out.st.facing, 4);
    check("gender", out.st.gender, 1);

    buf[3] ^= 0x10;
    got = 0;
    for (size_t i = 0; i < n; i++) got += mn_rx_byte(&rx, buf[i], &out);
    check("a corrupt one is dropped", got, 0);
    check("and counted", rx.bad, 1);

    mn_msg_t s = { .type = MN_STATE, .st = { .group = 1, .num = 4, .x = -3, .y = 20, .facing = 2 } };
    n = mn_encode(&s, buf, sizeof buf);
    got = 0;
    for (size_t i = 0; i < n; i++) got += mn_rx_byte(&rx, buf[i], &out);
    check("a state", got, 1);
    check("a negative x", out.st.x, -3);
}

/* ---- a model agent -------------------------------------------------------- */

#define MB 8
#define WALK_FRAMES 16

typedef struct { bool up; int x, y, facing, busy; uint8_t group, num; int gfx; } fig_t;

typedef struct {
    mmo_rx_t rx;
    mmo_msg_t mb[MB];
    int mbn;
    uint16_t received;
    int overflow;
    fig_t fig[MMO_MAX_PLAYERS];
    mmo_state_t me;
    uint16_t tx[64];
    int txh, txn;
    uint16_t said_received;
    int said_free;
    mmo_state_t said;
    bool said_once;
} agent_t;

static void agent_init(agent_t *a, mmo_state_t me) {
    memset(a, 0, sizeof *a);
    mmo_rx_init(&a->rx);
    a->me = me;
}

static void agent_tx(agent_t *a, const mmo_msg_t *m) {
    uint16_t w[MMO_MAX_WORDS + 2];
    int n = mmo_encode(m, w, MMO_MAX_WORDS + 2);
    for (int i = 0; i < n; i++) {
        a->tx[(a->txh + a->txn++) % 64] = w[i];
    }
}

static uint16_t agent_word(agent_t *a) {
    if (!a->txn) return MMO_IDLE;
    uint16_t w = a->tx[a->txh];
    a->txh = (a->txh + 1) % 64;
    a->txn--;
    return w;
}

static void agent_hear(agent_t *a, uint16_t w) {
    if (!mmo_rx_word(&a->rx, w)) return;
    if (a->mbn >= MB) {
        a->overflow++;
        return;
    }
    a->mb[a->mbn++] = a->rx.msg;
    a->received = (uint16_t)((a->received + 1) & 0x7FFF);
}

/* The game's frame: the mailbox run, the figures' walks counted down, a
 * state said if anything changed. */
static void agent_frame(agent_t *a) {
    for (int i = 0; i < MMO_MAX_PLAYERS; i++) {
        if (a->fig[i].busy) a->fig[i].busy--;
    }
    int k = 0;
    for (; k < a->mbn; k++) {
        mmo_msg_t *c = &a->mb[k];
        int lid = c->w[0] - MMO_LOCALID;
        if (lid < 0 || lid >= MMO_MAX_PLAYERS) continue;
        fig_t *f = &a->fig[lid];
        if (c->type == MMO_SPAWN) {
            if (!f->up && c->w[5] == (uint16_t)(a->me.group << 8 | a->me.num)) {
                *f = (fig_t){ .up = true, .x = (int16_t)c->w[2], .y = (int16_t)c->w[3], .facing = 1,
                              .group = a->me.group, .num = a->me.num, .gfx = c->w[1] };
            }
        } else if (c->type == MMO_MOVE) {
            if (!f->up) continue;
            if (f->busy) break;                 /* still walking: this and the rest wait */
            int act = c->w[1];
            if (act <= 3) {
                f->facing = act + 1;
            } else if (act >= 8 && act <= 11) {
                int d = act - 8 + 1;
                f->x += d == 4 ? 1 : d == 3 ? -1 : 0;
                f->y += d == 1 ? 1 : d == 2 ? -1 : 0;
                f->facing = d;
                f->busy = WALK_FRAMES;
            }
        } else if (c->type == MMO_REMOVE) {
            f->up = false;
        }
    }
    memmove(a->mb, a->mb + k, (size_t)(a->mbn - k) * sizeof a->mb[0]);
    a->mbn -= k;

    int free = MB - a->mbn;
    if (!a->said_once || memcmp(&a->said, &a->me, sizeof a->me) != 0 ||
        a->said_received != a->received || a->said_free != free) {
        mmo_msg_t s = { .type = MMO_STATE, .n = 6, .w = {
            (uint16_t)(a->me.group << 8 | a->me.num), (uint16_t)a->me.x, (uint16_t)a->me.y,
            (uint16_t)(a->me.facing | a->me.elevation << 4 | a->me.gender << 8),
            a->received, (uint16_t)free } };
        agent_tx(a, &s);
        a->said = a->me;
        a->said_received = a->received;
        a->said_free = free;
        a->said_once = true;
    }
}

/* The game changes map: every figure gone, the mailbox emptied. */
static void agent_warp(agent_t *a, uint8_t group, uint8_t num, int x, int y) {
    memset(a->fig, 0, sizeof a->fig);
    a->mbn = 0;
    a->me.group = group;
    a->me.num = num;
    a->me.x = (int16_t)x;
    a->me.y = (int16_t)y;
}

/* ---- the server, as the board sees it ------------------------------------ */

typedef struct { mn_msg_t sent[256]; int n; } server_t;

static void to_server(void *ctx, const mn_msg_t *m) {
    server_t *s = ctx;
    if (s->n < 256) s->sent[s->n++] = *m;
}

static void tell(mmo_relay_t *r, int pid, mmo_state_t st) {
    mn_msg_t m = { .type = MN_PLAYER, .pid = (uint8_t)pid, .st = st };
    mmo_relay_net(r, &m);
}

/* Frames of the game, each with up to `per` transfers through the board. */
static void run(mmo_relay_t *r, agent_t *a, int frames, int per) {
    for (int f = 0; f < frames; f++) {
        for (int t = 0; t < per; t++) {
            uint16_t back = mmo_relay_word(r, agent_word(a));
            agent_hear(a, back);
        }
        agent_frame(a);
    }
}

static bool fig_at(const agent_t *a, int pid, int x, int y, int facing) {
    const fig_t *f = &a->fig[pid];
    return f->up && f->x == x && f->y == y && (facing == 0 || f->facing == facing);
}

static void test_relay(void) {
    printf("the relay\n");
    server_t srv = {0};
    mmo_relay_t r;
    mmo_relay_init(&r, to_server, &srv);
    check("hello first", srv.sent[0].type, MN_HELLO);

    agent_t a;
    mmo_state_t town = { .group = 0, .num = 9, .x = 10, .y = 13, .facing = 1, .elevation = 3, .gender = 0 };
    agent_init(&a, town);
    mn_msg_t w = { .type = MN_WELCOME, .pid = 0, .max = 4 };
    mmo_relay_net(&r, &w);
    run(&r, &a, 4, 8);
    check("the board told the server where we are", srv.sent[srv.n - 1].type, MN_STATE);
    check("  x", srv.sent[srv.n - 1].st.x, 10);

    /* Player 1 arrives in town: a figure, facing their way. */
    mmo_state_t p1 = { .group = 0, .num = 9, .x = 12, .y = 13, .facing = 3, .elevation = 3, .gender = 1 };
    tell(&r, 1, p1);
    run(&r, &a, 6, 8);
    check("a figure for player 1", fig_at(&a, 1, 12, 13, 3), 1);
    check("  May, for a girl", a.fig[1].gfx, MMO_GFX_MAY);

    /* Five steps, as fast as the server says them: walked one by one. */
    for (int s = 0; s < 5; s++) {
        p1.y -= 1;
        p1.facing = 2;
        tell(&r, 1, p1);
    }
    run(&r, &a, 5 * WALK_FRAMES + 10, 8);
    check("five steps, walked", fig_at(&a, 1, 12, 8, 2), 1);
    check("no command lost to a full mailbox", a.overflow, 0);

    /* Twelve steps at once: more than the mailbox holds while the first is
     * walked. Only credits keep the board from overflowing it. */
    for (int s = 0; s < 12; s++) {
        p1.x += s < 6 ? 1 : -1;
        p1.facing = s < 6 ? 4 : 3;
        tell(&r, 1, p1);
    }
    run(&r, &a, 12 * WALK_FRAMES + 20, 8);
    check("twelve steps, walked", fig_at(&a, 1, 12, 8, 3), 1);
    check("the mailbox never overflowed", a.overflow, 0);

    /* A turn in place. */
    p1.facing = 4;
    tell(&r, 1, p1);
    run(&r, &a, 4, 8);
    check("turned in place", fig_at(&a, 1, 12, 8, 4), 1);

    /* A jump (a stall, then the latest): placed. */
    p1.x = 4;
    p1.y = 4;
    tell(&r, 1, p1);
    run(&r, &a, 6, 8);
    check("placed after a jump", fig_at(&a, 1, 4, 4, 0), 1);

    /* Player 2 elsewhere: not drawn here. */
    mmo_state_t p2 = { .group = 1, .num = 4, .x = 5, .y = 5, .facing = 1 };
    tell(&r, 2, p2);
    run(&r, &a, 4, 8);
    check("a player on another map is not drawn", a.fig[2].up, 0);

    /* We walk into the lab (player 2's map): figures gone, player 2 drawn. */
    agent_warp(&a, 1, 4, 6, 8);
    run(&r, &a, 6, 8);
    check("on the new map: player 2", fig_at(&a, 2, 5, 5, 0), 1);
    check("and not player 1, still in town", a.fig[1].up, 0);
    check("the server heard the new map", srv.sent[srv.n - 1].st.num, 4);

    /* Player 2 leaves the game. */
    mn_msg_t gone = { .type = MN_GONE, .pid = 2 };
    mmo_relay_net(&r, &gone);
    run(&r, &a, 4, 8);
    check("gone: removed", a.fig[2].up, 0);
    check("and forgotten", r.peer[2].known, 0);

    /* Back to town: player 1 drawn where they are now. */
    agent_warp(&a, 0, 9, 7, 8);
    run(&r, &a, 6, 8);
    check("back in town: player 1 where they are now", fig_at(&a, 1, 4, 4, 0), 1);

    /* Our own id is never drawn. */
    tell(&r, 0, town);
    run(&r, &a, 4, 8);
    check("our own player is not a figure", a.fig[0].up, 0);
    check("never more than the mailbox holds", a.overflow, 0);
}

/* Many players at once, at one transfer a frame: the queue backs up and
 * overflows, and everything still ends where the server says. */
static void test_crowd(void) {
    printf("a crowd, through a narrow link\n");
    server_t srv = {0};
    mmo_relay_t r;
    mmo_relay_init(&r, to_server, &srv);
    agent_t a;
    mmo_state_t town = { .group = 0, .num = 9, .x = 10, .y = 13, .facing = 1 };
    agent_init(&a, town);
    mn_msg_t w = { .type = MN_WELCOME, .pid = 0, .max = 16 };
    mmo_relay_net(&r, &w);
    run(&r, &a, 4, 4);

    mmo_state_t p[MMO_MAX_PLAYERS];
    for (int i = 1; i < MMO_MAX_PLAYERS; i++) {
        p[i] = (mmo_state_t){ .group = 0, .num = 9, .x = (int16_t)i, .y = 20, .facing = 2 };
        tell(&r, i, p[i]);
    }
    for (int s = 0; s < 6; s++) {                 /* everyone walks, fast */
        for (int i = 1; i < MMO_MAX_PLAYERS; i++) {
            p[i].y -= 1;
            tell(&r, i, p[i]);
        }
        run(&r, &a, 2, 1);
    }
    check("the queue overflowed", r.overflows > 0, 1);
    run(&r, &a, 2000, 1);
    int right = 0;
    for (int i = 1; i < MMO_MAX_PLAYERS; i++) {
        right += fig_at(&a, i, i, 14, 0);
    }
    check("every figure ends where its player is", right, MMO_MAX_PLAYERS - 1);
    check("the agent's mailbox never overflowed", a.overflow, 0);
    check("nothing left queued", mmo_relay_pending(&r), 0);
}

int main(void) {
    test_link();
    test_net();
    test_relay();
    test_crowd();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
