"""novaboot_adapter.py -- Pokemon Emeralds in one overworld, each through
its own emulated adapter: the in-game agent's link words go to the touch
carrier's own firmware on qemu_esp32 (the hybrid image, Link: NovaBoot,
reached by touch), whose relay (src/mmo_relay.c) talks to one server
(qemu/mmo_server.py). overworld_sync.py checked the same with the test
script in the board's place; this is the board doing its part.

    python3 -I test/mgba/novaboot_adapter.py <emerald.gba> <out-dir> [players]

players: 2 (the default) to 4, an emulated adapter for each.

What is real: the games (mGBA cores), the agent in each (a game Task,
test/mgba/mmo_agent.py: its player's MMO_STATE out, the board's commands
into its mailbox, run with the game's own functions), the board's firmware
and drivers (QEMU, the hybrid image), the link_proto bridge and the server
tunnel, the server. What stands in: the GBA's serial hardware -- this script
moves the agent's words between its EWRAM rings and the board's bridge,
six words each way a frame (the agent's own SIO driver is to come) -- and,
for the board, the server tunnel (a UART, where a board would use its
network). The town is quieted as overworld_sync.py quiets it.

Checked, on every game, for every other player:
  - their figure appears, on their tile, facing their way;
  - while they walk their scripted route, the figure is never more than one
    step behind;
  - once they stop, it stands on their tile facing their way, and is drawn
    (its sprite exists, is not hidden);
and: the agents' mailboxes never overflowed, no bad frame reached either
end, no word was resent; the screens and the boards' panels are saved.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'qemu'))
import emerald as E            # noqa: E402
import link4 as L4             # noqa: E402
import mmo_agent as A          # noqa: E402
import overworld_sync as OS    # noqa: E402
import mmo_adapter as MA       # noqa: E402
import mmo_server as MS        # noqa: E402

PER_FRAME = 6                  # link transfers a frame, each way

fails = 0


def check(what, ok, detail=None):
    global fails
    print('  %s  %s%s' % ('ok  ' if ok else 'FAIL', what, (' (%r)' % (detail,)) if detail is not None and not ok else ''))
    fails += 0 if ok else 1


def hook(g):
    """The agent's task, whenever the overworld runs without it (a map load
    resets the tasks)."""
    c, t = g.c, g.c.sym['gTasks']
    if c.callback2() != c.sym['CB2_Overworld'] | 1:
        return
    for k in range(16):
        if c.r8(t + 40 * k + 4) and c.r32(t + 40 * k) == A.AGENT | 1:
            return
    c.call_once(OS.CREATE, base=E.FREE_EWRAM + 0xE00, defines={'AGENT_ENTRY': hex(A.AGENT | 1)})


def link(games, boards):
    """The serial hardware's stand-in: words between each agent and its board."""
    for g, b in zip(games, boards):
        for _ in range(PER_FRAME):
            back = b.transfer(A.tx_pop(g.c))
            if not A.rx_push(g.c, back):
                raise RuntimeError('game %d: the agent\'s RX ring is full' % g.i)


def frame(m, games, boards, srv, keys):
    m.run(1, keys)
    OS.keep_quiet(games)
    for g in games:
        hook(g)
    link(games, boards)
    srv.poll(0)


def main(rom, out, n):
    os.makedirs(out, exist_ok=True)
    print('emerald overworld, %d players, each through an emulated adapter' % n)
    states, _ = L4.make_states(rom, out)
    m = E.Multi(rom, n)
    for i in range(n):
        m.core(i).load_state(states[i])
    games = [OS.Game(m, i) for i in range(n)]
    m.run(10)
    where = OS.warp_all(m, games, OS.SPOTS[:n])
    check('every player in Littleroot Town, on its own tile (%s)' % where,
          all(w[:2] == OS.LITTLEROOT for w in where) and len({w[2:] for w in where}) == n)
    code = A.build(m.sym)
    for g in games:
        A.install(g.c, code)
        hook(g)

    boards, srv = [], MS.Server()
    try:
        boards = [MA.Adapter(i, out) for i in range(n)]
        for b in boards:
            check('board %d boots' % b.i, b.wait_log('tasks started for the gate image', 120))
        time.sleep(2.5)
        for b in boards:
            srv.connect('127.0.0.1', b.net_port)
            check('board %d: Link: NovaBoot, by touch' % b.i, b.start_novaboot())
            check('board %d: its link is up' % b.i, b.connect_link())
        end = time.time() + 5
        while time.time() < end and not all(x.welcomed for x in srv.boards):
            srv.poll(0.05)
        check('every relay joined the server', all(x.welcomed for x in srv.boards))
        for b in boards:                       # the sessions idled during set-up
            b.transfer(0)
            b.resent = 0

        for _ in range(60):
            frame(m, games, boards, srv, [0] * n)
        seen = all(g.figure(k) for g in games for k in range(n) if k != g.i)
        check('every game shows every other player', seen,
              [[bool(g.figure(k)) for k in range(n)] for g in games])

        worst = 0
        routes = OS.ROUTES[:n]
        frames = max(sum(t for _, t in r) for r in routes) + 1
        for f in range(frames + 150):
            frame(m, games, boards, srv, [OS.keys_at(r, f) for r in routes])
            if f < frames:
                for g in games:
                    for k in range(n):
                        fig = g.figure(k) if k != g.i else None
                        if fig:
                            p = games[k].player()
                            worst = max(worst, abs(fig[0] - p[2]) + abs(fig[1] - p[3]))
        check('while walking, every figure within one step of its player (worst %d)' % worst, worst <= 1)

        for g in games:
            for k in range(n):
                if k == g.i:
                    continue
                p, fig = games[k].player(), g.figure(k)
                check('game %d shows player %d on their tile, facing their way, drawn' % (g.i, k),
                      fig is not None and fig[:3] == (p[2], p[3], p[4]) and not fig[4],
                      (fig, p[2:5]))
        counts = [A.counts(g.c) for g in games]
        check('no agent mailbox overflowed', not any(c['overflow'] for c in counts), counts)
        check('no bad frame reached an agent', not any(c['bad'] for c in counts), counts)
        check('none reached the server', srv.bad_frames() == 0, srv.bad_frames())
        check('no word had to be sent again', not any(b.resent for b in boards), [b.resent for b in boards])
        print('    words %s; server: %s in, %s out' % ([b.words for b in boards], srv.received, srv.sent))
        for g in games:
            g.c.shot(os.path.join(out, 'novaboot-adapter-%d.png' % g.i))
        for b in boards:
            b.shot(os.path.join(out, 'novaboot-adapter-board%d.ppm' % b.i))
    except (RuntimeError, TimeoutError) as e:
        check('ran to the end', False, '%s: %s' % (type(e).__name__, e))
    finally:
        for b in boards:
            b.stop()
    print('emerald overworld through emulated adapters: %s' % ('all checks passed' if not fails else 'FAILURES'))
    return 1 if fails else 0


if __name__ == '__main__':
    rom = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('EMERALD_ROM', '')
    if not rom or not os.path.exists(rom):
        print('emerald overworld through emulated adapters  skipped: no ROM')
        sys.exit(77)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(E.ROOT, 'build/emerald')
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    sys.exit(main(rom, out, n))
