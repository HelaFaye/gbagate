"""test_mmo_adapter.py -- the shared overworld through two emulated adapters:
two touch carriers (the hybrid image, the board's own firmware and drivers,
on qemu_esp32), each in Link: NovaBoot (reached by touch), each with a
model GBA on its link, both through one server (qemu/mmo_server.py).

    python3 -I qemu/test_mmo_adapter.py [out-dir]

The model GBA keeps the in-game agent's contract (test/mgba/mmo_agent.py,
and test/test_mmo.c's model): its player's MMO_STATE out, the board's
commands into a mailbox of eight, run in order, a walk holding back what
follows it for 16 frames, a spawn only for its own map, the mailbox emptied
on a change of map. The words go through the real link_proto bridge on
UART1, the relay (src/mmo_relay.c) in the firmware, and the server tunnel on
UART2. test/mgba/novaboot_adapter.py does the same with Emerald.

Checked:
  - each board offers Link: NovaBoot (it has a server), and its relay joins
    the server;
  - each GBA sees the other's figure where they stand, facing their way;
  - one walks and turns: the other's figure follows, step for step, and ends
    on their tile facing their way;
  - one goes to another map: the other's figure of them goes, and comes back
    where they are when they return;
  - nothing lost: no mailbox overflow, no bad frame at either end, no
    resent word, nothing refused by the server.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mmo_adapter as MA     # noqa: E402
import mmo_server as MS      # noqa: E402

fails = 0


def check(what, ok, detail=''):
    global fails
    print('  %s  %s%s' % ('ok  ' if ok else 'FAIL', what, (' (%r)' % (detail,)) if detail is not None and detail != '' and not ok else ''))
    fails += 0 if ok else 1


# ---- mmo_link in Python (src/mmo_link.c's rules) ---------------------------

def encode(mtype, words):
    hdr = 0xA500 | mtype << 4 | len(words)
    s = (0x5A5A + hdr + sum(words)) & 0xFFFF
    return [hdr] + list(words) + [0xFFFE if s == 0xFFFF else s]


class LinkReader:
    def __init__(self):
        self.have, self.bad = 0, 0

    def word(self, w):
        """A frame (type, words) when w completes one."""
        if self.have == 0:
            if w in (0, 0xFFFF):
                return None
            if w & 0xFF00 != 0xA500 or not w & 0xF0:
                self.bad += 1
                return None
            self.type, self.n, self.sum, self.w, self.have = (w >> 4) & 15, w & 15, (0x5A5A + w) & 0xFFFF, [], 1
            return None
        if w == 0xFFFF:
            self.bad += 1
            self.have = 0
            return None
        if self.have <= self.n:
            self.w.append(w)
            self.sum = (self.sum + w) & 0xFFFF
            self.have += 1
            return None
        self.have = 0
        if w != (0xFFFE if self.sum == 0xFFFF else self.sum):
            self.bad += 1
            if w & 0xFF00 == 0xA500 and w & 0xF0:
                return self.word(w)
            return None
        return self.type, self.w


# ---- a model GBA -------------------------------------------------------------

class ModelGBA:
    MB, WALK = 8, 16

    def __init__(self, group, num, x, y, facing, gender):
        self.map, self.x, self.y, self.facing, self.gender = (group, num), x, y, facing, gender
        self.elev = 3
        self.rx = LinkReader()
        self.mb, self.received, self.overflow = [], 0, 0
        self.fig = {}                     # lid -> [x, y, facing, busy]
        self.tx = []
        self.said = None

    def word_out(self):
        return self.tx.pop(0) if self.tx else 0

    def word_in(self, w):
        f = self.rx.word(w)
        if not f:
            return
        self.received = (self.received + 1) & 0x7FFF
        if len(self.mb) >= self.MB:
            self.overflow += 1
            return
        self.mb.append(f)

    def frame(self):
        for f in self.fig.values():
            f[3] = max(0, f[3] - 1)
        k = 0
        while k < len(self.mb):
            t, w = self.mb[k]
            lid = w[0]
            if t == 3:                                  # spawn, on its own map only
                if lid not in self.fig and w[5] == (self.map[0] << 8 | self.map[1]):
                    self.fig[lid] = [w[2], w[3], 1, 0]
            elif t == 4:
                f = self.fig.get(lid)
                if f:
                    if f[3]:
                        break                           # walking: this and the rest wait
                    a = w[1]
                    if a <= 3:
                        f[2] = a + 1
                    elif 8 <= a <= 11:
                        d = a - 7
                        f[0] += 1 if d == 4 else -1 if d == 3 else 0
                        f[1] += 1 if d == 1 else -1 if d == 2 else 0
                        f[2], f[3] = d, self.WALK
            elif t == 5:
                self.fig.pop(lid, None)
            k += 1
        self.mb = self.mb[k:]
        st = [self.map[0] << 8 | self.map[1], self.x & 0xFFFF, self.y & 0xFFFF,
              self.facing | self.elev << 4 | self.gender << 8, self.received, self.MB - len(self.mb)]
        if st != self.said and len(self.tx) < 24:
            self.tx += encode(1, st)
            self.said = st

    def warp(self, group, num, x, y):
        self.map, self.x, self.y = (group, num), x, y
        self.fig, self.mb = {}, []

    def figure(self, pid):
        f = self.fig.get(0x40 + pid)
        return tuple(f[:3]) if f else None


# ---- the run ------------------------------------------------------------------

def frames(boards, gbas, srv, n, per=6, step=None):
    for f in range(n):
        if step:
            step(f)
        for b, g in zip(boards, gbas):
            for _ in range(per):
                g.word_in(b.transfer(g.word_out()))
            g.frame()
        srv.poll(0)


def main(out):
    os.makedirs(out, exist_ok=True)
    print('the shared overworld through two emulated adapters (qemu_esp32), model GBAs')
    boards, srv = [], MS.Server()
    try:
        boards = [MA.Adapter(i, out) for i in range(2)]
        for b in boards:
            check('board %d boots' % b.i, b.wait_log('tasks started for the gate image', 90))
        time.sleep(2.5)
        for b in boards:
            caps = b.caps() or 0
            check('board %d has a way to the server, and says it is a stand-in (caps %#x)' % (b.i, caps),
                  bool(caps & (1 << 13)) and 'stand-in for the board' in b.log())
            srv.connect('127.0.0.1', b.net_port)
            check('board %d: Link: NovaBoot, by touch' % b.i, b.start_novaboot())
            check('board %d: its link is up' % b.i, b.connect_link())
        end = time.time() + 5
        while time.time() < end and not all(x.welcomed for x in srv.boards):
            srv.poll(0.05)
        check('both relays said hello and were welcomed', all(x.welcomed for x in srv.boards))

        # Each link idled while the other board was set up, and the bridge
        # drops a silent session: one transfer each brings it back. Words
        # resent from here on are counted.
        for b in boards:
            b.transfer(0)
            b.resent = 0

        town = (0, 9)
        gbas = [ModelGBA(*town, 10, 13, 1, 1), ModelGBA(*town, 12, 13, 3, 0)]
        frames(boards, gbas, srv, 40)
        check('A sees B where B stands, facing B\'s way', gbas[0].figure(1) == (12, 13, 3), gbas[0].figure(1))
        check('B sees A', gbas[1].figure(0) == (10, 13, 1), gbas[1].figure(0))

        # B walks: up four, a turn east, left two; one step each 16 frames.
        route = [('y', -1)] * 4 + [('turn', 4)] + [('x', -1)] * 2

        def walk(f):
            k = f // 16
            if f % 16 or k >= len(route):
                return
            what, v = route[k]
            g = gbas[1]
            if what == 'turn':
                g.facing = v
            elif what == 'x':
                g.x += v
                g.facing = 3 if v < 0 else 4
            else:
                g.y += v
                g.facing = 2 if v < 0 else 1
            walk.lag = max(walk.lag, lag())

        def lag():
            f = gbas[0].figure(1)
            return abs(f[0] - gbas[1].x) + abs(f[1] - gbas[1].y) if f else 99
        walk.lag = 0
        frames(boards, gbas, srv, 16 * len(route) + 60, step=walk)
        check('B walked: A\'s figure of B ends on B\'s tile, facing B\'s way',
              gbas[0].figure(1) == (gbas[1].x, gbas[1].y, gbas[1].facing),
              '%s vs %s' % (gbas[0].figure(1), (gbas[1].x, gbas[1].y, gbas[1].facing)))
        check('and never more than a step behind while walking (%d)' % walk.lag, walk.lag <= 1)

        # A goes into a building, then back out.
        gbas[0].warp(1, 4, 6, 8)
        frames(boards, gbas, srv, 40)
        check('A indoors: B no longer sees A', gbas[1].figure(0) is None, gbas[1].figure(0))
        check('and A sees no one there', not gbas[0].fig, gbas[0].fig)
        gbas[0].warp(0, 9, 7, 8)
        frames(boards, gbas, srv, 40)
        check('A back in town: B sees A at the door', gbas[1].figure(0) == (7, 8, 1), gbas[1].figure(0))
        check('and A sees B where B is', gbas[0].figure(1) == (gbas[1].x, gbas[1].y, gbas[1].facing),
              gbas[0].figure(1))

        check('no agent mailbox overflowed', not any(g.overflow for g in gbas),
              [g.overflow for g in gbas])
        check('no bad frame reached a GBA', not any(g.rx.bad for g in gbas), [g.rx.bad for g in gbas])
        check('none reached the server', srv.bad_frames() == 0, srv.bad_frames())
        check('no word had to be sent again', not any(b.resent for b in boards), [b.resent for b in boards])
        print('    %s words; server: %s in, %s out' % ([b.words for b in boards], srv.received, srv.sent))
        for b in boards:
            b.shot(os.path.join(out, 'board%d-novaboot.ppm' % b.i))
    except Exception as e:                       # noqa: BLE001
        check('ran to the end', False, '%s: %s' % (type(e).__name__, e))
    finally:
        for b in boards:
            b.stop()
    print('overworld through two emulated adapters: %s' % ('all checks passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(MA.ROOT, 'build/mmo-adapter')))
