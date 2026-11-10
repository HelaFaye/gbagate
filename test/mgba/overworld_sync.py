"""overworld_sync.py -- four Pokemon Emeralds in one overworld: each player
walks with emulated controls, and every other player's screen shows them
walking, in the right place, facing the right way (doc/MMO.md, step 3).

    python3 -I test/mgba/overworld_sync.py <emerald.gba> <out-dir>

Four new games (link4.py's), each warped into Littleroot Town on its own
tile with the game's own warp. The town is quieted as the warp lands, in
the RAM copy of its header (as union_room.py does): no map scripts, no
townspeople, no step-on triggers (the intro's move the player); the doors
stay. Then on each game:

  - an agent stand-in: a game Task (CreateTask), which the game's own
    RunTasks calls every frame, that runs a mailbox of commands in free EWRAM
    with the game's own functions: SpawnSpecialObjectEventParameterized
    for a remote player (Brendan or May, by the player's own gender),
    ObjectEventSetHeldMovement to walk or turn it, RemoveObjectEvent...;
  - a server stand-in (this script): every frame it reads each player's map,
    tile, facing and elevation, and when a player steps or turns it queues
    that step for the player's figure on every other game on the same map.
    Steps play back in order, each when the last has finished: doc/MMO.md's
    "a short delay, not prediction".

Each player walks a scripted route (steps in every direction, turns in
place, walking into walls). Checked, on every game, for every other player:
  - while they walk, the figure is never more than one step behind;
  - once they stop, the figure stands on the player's tile and faces their
    way;
  - it is drawn: its sprite exists, is not hidden, and is on screen where
    the two players' positions say (each game's camera centres its player);
  - the screens are saved, one per player, for a person to look at.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import emerald as E      # noqa: E402
import link4 as L4       # noqa: E402

N = 4
LITTLEROOT = (0, 9)                       # pokeemerald data/maps/map_groups.json
SPOTS = ((10, 13), (12, 13), (10, 15), (12, 15))   # each player's tile in the town
LOCALID = 0x40                            # remote player i is local id LOCALID + i
# The 4 KB of EWRAM past pokeemerald's data (0x0203F000-0x0203FFFF); past
# its end is a mirror of 0x02000000, the game's heap:
#   +0x000 call_once's scratch   +0x800 an empty map script table
#   +0x840 the quieted events    +0x900 the agent stand-in
#   +0xD00 its mailbox           +0xE00 the task-creating stub
AGENT = E.FREE_EWRAM + 0x900
AGENT_MAX = 0x400
MAILBOX = E.FREE_EWRAM + 0xD00            # u16 count, then 8 commands of 6 u16
MB_MAX = 8
OBJ = 0x24                                # sizeof(struct ObjectEvent)

GFX_BRENDAN, GFX_MAY = 0, 89              # OBJ_EVENT_GFX_*_NORMAL
WALK = {'down': 0x08, 'up': 0x09, 'left': 0x0A, 'right': 0x0B}   # MOVEMENT_ACTION_WALK_NORMAL_*
FACE = {1: 0x00, 2: 0x01, 3: 0x02, 4: 0x03}                      # DIR_* -> MOVEMENT_ACTION_FACE_*
DIRS = {1: 'down', 2: 'up', 3: 'left', 4: 'right'}               # DIR_SOUTH.. DIR_EAST
C_SPAWN, C_MOVE, C_REMOVE = 1, 2, 3

WARP = '''
typedef void (*fn)(void);
typedef void (*warp_fn)(int, int, int, int, int);
void entry(void) {
    ((warp_fn)(SYM_SetWarpDestination | 1))(MAP_GROUP, MAP_NUM, -1, X, Y);
    ((fn)(SYM_DoWarp | 1))();
    *(volatile unsigned *)(SYM_gMain + 4) = SYM_CB2_Overworld | 1;
    ((fn)(SYM_CB2_Overworld | 1))();
}
'''

# Not as gMain.callback2: the overworld's input (CB1_Overworld) only runs
# while callback2 is CB2_Overworld itself, so a hook there freezes the
# player. A Task is called every frame by the game's RunTasks.
CREATE = '''
typedef void (*fn)(void);
typedef unsigned char (*create_fn)(void *, unsigned char);
void entry(void) {
    ((create_fn)(SYM_CreateTask | 1))((void *)(AGENT_ENTRY), 80);
    *(volatile unsigned *)(SYM_gMain + 4) = SYM_CB2_Overworld | 1;
    ((fn)(SYM_CB2_Overworld | 1))();
}
'''

# The agent stand-in. Commands run in order; a step for a figure still
# walking stays, with everything after it, for the next frame.
AGENT_SRC = '''
typedef unsigned char u8;
typedef short s16;
typedef void (*fn)(void);
typedef u8 (*spawn_fn)(u8, u8, u8, s16, s16, u8);
typedef u8 (*id_fn)(u8, u8, u8);
typedef u8 (*u8obj_fn)(void *);
typedef u8 (*set_fn)(void *, u8);
typedef void (*rm_fn)(void *);
#define MB ((volatile unsigned short *)MAILBOX)
void entry(u8 task) {
    (void)task;
    const u8 *sb = *(u8 **)SYM_gSaveBlock1Ptr;
    const u8 group = sb[4], num = sb[5];
    int n = MB[0], k = 0;
    for (; k < n; k++) {
        volatile unsigned short *c = MB + 1 + 6 * k;
        const u8 lid = (u8)c[1];
        const u8 id = ((id_fn)(SYM_GetObjectEventIdByLocalIdAndMap | 1))(lid, num, group);
        u8 *obj = (u8 *)SYM_gObjectEvents + 0x24 * id;
        if (c[0] == 1) {                       /* spawn: gfx, x, y, elevation */
            if (id == 16)
                ((spawn_fn)(SYM_SpawnSpecialObjectEventParameterized | 1))((u8)c[2], 0, lid, (s16)c[3], (s16)c[4], (u8)c[5]);
        } else if (c[0] == 2) {                /* a held movement, once the last one is done */
            if (id == 16) continue;
            const u8 st = ((u8obj_fn)(SYM_ObjectEventClearHeldMovementIfFinished | 1))(obj);
            if (st == 0) break;                /* still walking: this and the rest wait */
            ((set_fn)(SYM_ObjectEventSetHeldMovement | 1))(obj, (u8)c[2]);
        } else if (c[0] == 3) {                /* remove */
            if (id != 16) ((rm_fn)(SYM_RemoveObjectEvent | 1))(obj);
        }
    }
    for (int i = k; i < n; i++)                /* what is left, to the front */
        for (int w = 0; w < 6; w++) MB[1 + 6 * (i - k) + w] = MB[1 + 6 * i + w];
    MB[0] = (unsigned short)(n - k);
}
'''

fails = 0


def check(what, ok):
    global fails
    print('  %s  %s' % ('ok  ' if ok else 'FAIL', what))
    fails += 0 if ok else 1


class Game:
    def __init__(self, m, i):
        self.m, self.i, self.c = m, i, m.core(i)
        self.queue = []                      # commands not yet in the mailbox
        self.shown = {}                      # remote player -> last (x, y, facing) given

    def player(self):
        """(map group, map num, x, y, facing, elevation, gender) of this game's player."""
        c, s = self.c, self.c.sym
        sb = c.r32(s['gSaveBlock1Ptr'])
        oid = c.r8(s['gPlayerAvatar'] + 5)
        ob = s['gObjectEvents'] + OBJ * oid
        x, y = struct.unpack('<hh', c.read(ob + 0x10, 4))
        return (c.r8(sb + 4), c.r8(sb + 5), x, y, c.r8(ob + 0x18) & 0x0F, c.r8(ob + 0x0B) & 0x0F,
                c.r8(s['gPlayerAvatar'] + 7))

    def figure(self, k):
        """Remote player k's figure here: (x, y, facing, sprite id, invisible, offscreen, walking), or None."""
        c, s = self.c, self.c.sym
        for oid in range(16):
            ob = s['gObjectEvents'] + OBJ * oid
            if c.r8(ob) & 1 and c.r8(ob + 8) == LOCALID + k:
                x, y = struct.unpack('<hh', c.read(ob + 0x10, 4))
                b0, b1 = c.r8(ob), c.r8(ob + 1)
                return (x, y, c.r8(ob + 0x18) & 0x0F, c.r8(ob + 4), bool(b1 & 0x20), bool(b1 & 0x40),
                        bool(b0 & 0x40) and not bool(b0 & 0x80))
        return None

    def send(self, *cmd):
        self.queue.append(list(cmd) + [0] * (6 - len(cmd)))

    def flush(self):
        """Into the mailbox, as much as fits after what is still there."""
        c = self.c
        n = c.r16(MAILBOX)
        while self.queue and n < MB_MAX:
            for w, v in enumerate(self.queue.pop(0)):
                c.w16(MAILBOX + 2 + 12 * n + 2 * w, v)
            n += 1
        c.w16(MAILBOX, n)

    def pending(self):
        return len(self.queue) + self.c.r16(MAILBOX)

    def hook(self):
        """The agent stand-in's task, whenever the overworld runs without it
        (a map load resets the tasks)."""
        c, t = self.c, self.c.sym['gTasks']
        if c.callback2() != c.sym['CB2_Overworld'] | 1:
            return
        for k in range(16):
            if c.r8(t + 40 * k + 4) and c.r32(t + 40 * k) == AGENT | 1:
                return
        c.call_once(CREATE, base=E.FREE_EWRAM + 0xE00, defines={'AGENT_ENTRY': hex(AGENT | 1)})


QUIET_EVENTS = E.FREE_EWRAM + 0x840     # a copy of the map's events: warps only


def quiet(g, mh):
    """The town as a test needs it, set in the RAM header the frame the game
    copies it: no map scripts, no townspeople and no step-on triggers (the
    intro's, which move the player), the doors kept."""
    c = g.c
    ev = c.r32(mh + 4)
    if ev != QUIET_EVENTS and ev >= 0x08000000:
        hdr = bytearray(c.read(ev, 20))
        hdr[0] = 0                             # objectEventCount
        hdr[2] = 0                             # coordEventCount
        c.write(QUIET_EVENTS, bytes(hdr))
        c.w32(mh + 4, QUIET_EVENTS)
    c.w32(mh + 8, E.FREE_EWRAM + 0x800)        # an empty map script table


def warp_all(m, games):
    """Every player into Littleroot Town on its own tile, quieted."""
    mh = m.sym['gMapHeader']
    scripts = m.sym['LittlerootTown_MapScripts']
    for g, (x, y) in zip(games, SPOTS):
        g.c.w32(E.FREE_EWRAM + 0x800, 0)
        g.c.call_once(WARP, defines={'MAP_GROUP': LITTLEROOT[0], 'MAP_NUM': LITTLEROOT[1], 'X': x, 'Y': y})
    for _ in range(400):
        m.run(1)
        for g in games:
            if g.c.r32(mh + 8) == scripts:
                quiet(g, mh)
    return [g.player()[:4] for g in games]


class Server:
    """Who is where, and what each game must be told."""

    def __init__(self, games):
        self.games = games
        self.last = [None] * len(games)
        self.steps = [0] * len(games)            # each player's steps and turns, as seen
        self.turns = [0] * len(games)

    def tick(self):
        for k, g in enumerate(self.games):
            now = g.player()
            before = self.last[k]
            self.last[k] = now
            if before and before[:2] == now[:2]:
                if before[2:4] != now[2:4]:
                    self.steps[k] += 1
                elif before[4] != now[4]:
                    self.turns[k] += 1
            for j, h in enumerate(self.games):
                if j == k:
                    continue
                if h.player()[:2] != now[:2]:
                    if k in h.shown:                       # left this map
                        h.send(C_REMOVE, LOCALID + k)
                        del h.shown[k]
                    continue
                if k not in h.shown:                       # new here: spawn, then face
                    h.send(C_SPAWN, LOCALID + k, GFX_MAY if now[6] else GFX_BRENDAN, now[2], now[3], now[5])
                    h.send(C_MOVE, LOCALID + k, FACE[now[4]])
                    h.shown[k] = now[2:5]
                    continue
                sx, sy, sf = h.shown[k]
                dx, dy = now[2] - sx, now[3] - sy
                if (dx, dy) != (0, 0):
                    if abs(dx) + abs(dy) == 1:            # one step
                        h.send(C_MOVE, LOCALID + k, WALK['right' if dx > 0 else 'left' if dx < 0 else
                                                         'down' if dy > 0 else 'up'])
                    else:                                  # more (a stall, a warp): place it
                        h.send(C_REMOVE, LOCALID + k)
                        h.send(C_SPAWN, LOCALID + k, GFX_MAY if now[6] else GFX_BRENDAN, now[2], now[3], now[5])
                    h.shown[k] = (now[2], now[3], now[4])
                elif now[4] != sf and before and before[2:4] == now[2:4]:   # turned in place
                    h.send(C_MOVE, LOCALID + k, FACE[now[4]])
                    h.shown[k] = (sx, sy, now[4])
        for g in self.games:
            g.flush()


# Each player's route: (key, frames held). 16 frames a step when walking;
# a 2-frame tap turns in place; into a wall or another player is a bump.
ROUTES = [
    [(E.UP, 32), (0, 6), (E.LEFT, 48), (0, 6), (E.DOWN, 2), (0, 10), (E.RIGHT, 16), (0, 4), (E.DOWN, 32),
     (0, 4), (E.RIGHT, 32)],     # (ends in the open: behind the house the roof covers sprites)
    [(E.RIGHT, 32), (0, 8), (E.UP, 48), (0, 4), (E.LEFT, 2), (0, 12), (E.LEFT, 32), (0, 6), (E.DOWN, 16)],
    [(E.DOWN, 16), (0, 4), (E.RIGHT, 48), (0, 10), (E.UP, 2), (0, 8), (E.UP, 48), (E.LEFT, 16)],
    [(E.LEFT, 2), (0, 6), (E.LEFT, 16), (0, 6), (E.DOWN, 32), (0, 6), (E.RIGHT, 32), (0, 2), (E.UP, 2)],
]


def keys_at(route, f):
    t = 0
    for key, n in route:
        if f < t + n:
            return key
        t += n
    return 0


def main(rom, out):
    os.makedirs(out, exist_ok=True)
    print('emerald overworld, four players: walking, seen by each other')
    states, _ = L4.make_states(rom, out)
    m = E.Multi(rom, N)
    for i, st in enumerate(states):
        m.core(i).load_state(st)
    games = [Game(m, i) for i in range(N)]
    m.run(10)
    where = warp_all(m, games)
    check('every player in Littleroot Town, on its own tile (%s)' % where,
          all(w[:2] == LITTLEROOT for w in where) and len({w[2:] for w in where}) == N)
    code = E.build_thumb(AGENT_SRC, AGENT, m.sym, {'MAILBOX': hex(MAILBOX)})
    assert len(code) <= AGENT_MAX, 'the agent stand-in outgrew its room (%d bytes)' % len(code)
    for g in games:
        g.c.write(AGENT, code)
        g.c.w16(MAILBOX, 0)
        g.hook()
    srv = Server(games)
    for _ in range(30):                           # everyone spawned on everyone's game
        srv.tick()
        m.run(1)
        for g in games:
            g.hook()
    seen = all(g.figure(k) for g in games for k in range(N) if k != g.i)
    check('every game shows the three others', seen)

    worst = 0
    frames = max(sum(n for _, n in r) for r in ROUTES) + 1
    for f in range(frames + 120):
        m.run(1, [keys_at(r, f) for r in ROUTES])
        for g in games:
            g.hook()
        srv.tick()
        for g in games:                           # while walking: never more than a step behind
            for k in range(N):
                if k == g.i:
                    continue
                fig, p = g.figure(k), games[k].player()
                if fig:
                    worst = max(worst, abs(fig[0] - p[2]) + abs(fig[1] - p[3]))
    check('while walking, every figure within one step of its player (worst %d)' % worst, worst <= 1)
    for _ in range(60):                           # settle: the queues drain
        m.run(1)
        for g in games:
            g.hook()
        srv.tick()
    check('every queue drained', all(g.pending() == 0 for g in games))

    check('every player walked and turned (steps %s, turns %s)' % (srv.steps, srv.turns),
          all(srv.steps) and all(srv.turns))
    for g in games:
        me = g.player()
        for k in range(N):
            if k == g.i:
                continue
            p, fig = games[k].player(), g.figure(k)
            if not fig:
                check('game %d shows player %d' % (g.i, k), False)
                continue
            check('game %d: player %d at %s facing %s, figure at %s facing %s' %
                  (g.i, k, p[2:4], DIRS.get(p[4]), fig[:2], DIRS.get(fig[2])),
                  fig[:2] == p[2:4] and fig[2] == p[4])
            dx, dy = (p[2] - me[2]) * 16, (p[3] - me[3]) * 16
            on_screen = abs(dx) <= 112 and abs(dy) <= 72
            check('... drawn: sprite %d, %s, %s' % (fig[3], 'hidden' if fig[4] else 'shown',
                                                    'on screen' if on_screen and not fig[5] else 'off screen'),
                  fig[3] < 64 and not fig[4] and (fig[5] == (not on_screen)))
        g.c.shot(os.path.join(out, 'overworld-player%d.png' % g.i))
    print('emerald overworld, four players  %s' % ('all checks passed' if not fails else 'FAILURES'))
    return 1 if fails else 0


if __name__ == '__main__':
    rom = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('EMERALD_ROM', '')
    if not rom or not os.path.exists(rom):
        print('emerald overworld  skipped: no ROM')
        sys.exit(77)
    sys.exit(main(rom, sys.argv[2] if len(sys.argv) > 2 else os.path.join(E.ROOT, 'build/emerald')))
