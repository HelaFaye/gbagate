"""link4_online.py -- four Pokemon Emeralds in one Multi-Play link over the
internet: boards with one to three GBAs each, through the real Celio-Server.

    python3 -I test/mgba/link4_online.py <emerald.gba> <out-dir> [layout ...]

A layout is the GBAs on each board, e.g. 1111 (one GBA on each of four
boards: from any board, one local and three remote), 31 (three on one board,
one on another), 22. With none, all three run, each in its own process.

What is real: the games (mGBA cores, lockstep, mgba_multi.c's board mode),
each board's Celio client in its multi mode (src/celio.c), the Multi-Play
glue and relay (src/mp_online.c, src/mp_link.c, src/mp_relay.c), and the
server: Celio-Server with backport/Celio-Link-Server applied, built by
test/gblink_compat/run.sh (build/gblink_compat/celio_server_patched.mjs).
This script is each board's network driver: one WebSocket per board,
carrying its frames. What is modelled: each board's cable, per transfer of
all four slots (test/test_mpbus.c checks the electrical layer under it).

The checks are link4.py's, which four Emeralds pass on one real cable:
every game sees four players, only the parent is master, each knows its
link ID, the games exchange their link player records and every game holds
the same four trainer IDs in link order, with no link errors. And the
boards': no relay saw a bad checksum from its GBAs, nothing was dropped or
refused.
"""
import base64
import ctypes
import os
import select
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import emerald as E      # noqa: E402
import link4 as L4       # noqa: E402

N = 4
PORT = 443
STATE = ['idle', 'connecting', 'entering', 'waiting', 'starting', 'linked', 'closed', 'failed']

fails = 0


def check(what, ok):
    global fails
    print('  %s  %s' % ('ok  ' if ok else 'FAIL', what))
    fails += 0 if ok else 1


class WS:
    """Just enough of a WebSocket client (RFC 6455) for Socket.IO's text frames."""

    def __init__(self, host, port, path):
        self.s = socket.create_connection((host, port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(('GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                        'Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' % (path, host, port, key)).encode())
        head = b''
        while b'\r\n\r\n' not in head:
            chunk = self.s.recv(4096)
            if not chunk:
                raise ConnectionError('no upgrade')
            head += chunk
        if b' 101 ' not in head.split(b'\r\n', 1)[0]:
            raise ConnectionError(head.split(b'\r\n', 1)[0].decode())
        self.buf = head.split(b'\r\n\r\n', 1)[1]
        self.closed = False
        self.s.settimeout(None)

    def _frame(self, op, payload):
        n = len(payload)
        h = bytes([0x80 | op])
        if n < 126:
            h += bytes([0x80 | n])
        elif n < 65536:
            h += bytes([0x80 | 126]) + struct.pack('>H', n)
        else:
            h += bytes([0x80 | 127]) + struct.pack('>Q', n)
        mask = os.urandom(4)
        self.s.sendall(h + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def send(self, text):
        if not self.closed:
            self._frame(1, text)

    def recv(self):
        """The text frames that have arrived, without waiting."""
        out = []
        while select.select([self.s], [], [], 0)[0]:
            chunk = self.s.recv(65536)
            if not chunk:
                self.closed = True
                break
            self.buf += chunk
        while len(self.buf) >= 2:
            op, n = self.buf[0] & 0x0F, self.buf[1] & 0x7F
            i = 2
            if n == 126:
                if len(self.buf) < 4:
                    break
                n, i = struct.unpack('>H', self.buf[2:4])[0], 4
            elif n == 127:
                if len(self.buf) < 10:
                    break
                n, i = struct.unpack('>Q', self.buf[2:10])[0], 10
            if len(self.buf) < i + n:
                break
            payload, self.buf = self.buf[i:i + n], self.buf[i + n:]
            if op == 1:
                out.append(payload)
            elif op == 9:
                self._frame(10, payload)
            elif op == 8:
                self.closed = True
        return out


def server(out_dir):
    js = os.path.join(E.ROOT, 'build/gblink_compat/celio_server_patched.mjs')
    if not os.path.exists(js):
        return None
    log = open(os.path.join(out_dir, 'link4-online-server.log'), 'w')
    p = subprocess.Popen(['node', js], stdout=log, stderr=subprocess.STDOUT)
    for _ in range(150):
        try:
            socket.create_connection(('127.0.0.1', PORT), timeout=0.2).close()
            return p
        except OSError:
            time.sleep(0.1)
    p.kill()
    return None


def main(rom, out, layout):
    gbas = [int(c) for c in layout]
    print('emerald link over Celio, boards %s' % '+'.join(map(str, gbas)))
    os.makedirs(out, exist_ok=True)
    states, ids = L4.make_states(rom, out)
    srv = server(out)
    if not srv:
        print('  skipped: no Celio-Server bundle (run test/gblink_compat/run.sh)')
        return 77
    m = E.Multi(rom, N)
    lib = m.lib
    lib.m_board_ws_take.restype = ctypes.c_size_t
    for i, st in enumerate(states):
        m.core(i).load_state(st)
    if lib.m_board_mode(len(gbas), (ctypes.c_int * len(gbas))(*gbas)) != 0:
        print('  FAIL bad layout')
        return 1
    m.attach()
    m.run(30)

    socks = {}
    buf = ctypes.create_string_buffer(4096)

    def view(b):
        v = (ctypes.c_uint32 * 15)()
        sid = ctypes.create_string_buffer(40)
        lib.m_board_view(b, v, sid)
        keys = ('state', 'players', 'multi', 'bus', 'first', 'locals', 'transfers', 'frames', 'relayed',
                'received', 'bad_checksums', 'dropped', 'lost', 'bad_packets', 'out_lost')
        d = dict(zip(keys, list(v)))
        d['state'] = STATE[d['state']] if d['state'] < len(STATE) else d['state']
        d['session'] = sid.value.decode()
        return d

    def start(b, session):
        lib.m_board_start(b, session.encode(), N)
        socks[b] = WS('127.0.0.1', PORT, '/socket.io/?EIO=4&transport=websocket')
        lib.m_board_ws_open(b)

    def pump(frames=1):
        m.run(frames)
        for b, ws in socks.items():
            while True:
                n = lib.m_board_ws_take(b, buf, 4096)
                if not n:
                    break
                ws.send(buf.raw[:n])
            for t in ws.recv():
                lib.m_board_ws_text(b, t, len(t))

    def until(pred, seconds, what):
        end = time.time() + seconds
        while time.time() < end:
            pump()
            for b in socks:
                if view(b)['state'] in ('failed', 'closed'):
                    raise RuntimeError('board %d %s' % (b, view(b)['state']))
            if pred():
                return True
        raise RuntimeError('timed out: ' + what)

    try:
        start(0, '')
        until(lambda: view(0)['state'] == 'waiting', 30, 'board 0 in a session')
        sid = view(0)['session']
        for b in range(1, len(gbas)):
            start(b, sid)
        until(lambda: all(view(b)['bus'] for b in range(len(gbas))), 60, 'every board given a role')
        views = [view(b) for b in range(len(gbas))]
        for b, v in enumerate(views):
            print('    board %d: %d GBA(s), slots %d-%d' % (b, gbas[b], v['first'], v['first'] + v['locals'] - 1))
        slots = [lib.m_core_slot(i) for i in range(N)]
        check('every GBA has its own slot (%s)' % slots, sorted(slots) == list(range(N)))

        for i in range(N):
            m.core(i).call_once(L4.CALL % 'OpenLink')
        until(lambda: all(L4.status(m.core(i))['count'] == N for i in range(N)), 90, 'every game counting four')
        st = [L4.status(m.core(i)) for i in range(N)]
        check('every game sees four players', all(s['count'] == N for s in st))
        check('only the parent is master', [s['master'] for s in st] == [slots[i] == 0 for i in range(N)])
        check('each game knows its own link ID (%s)' % [s['local'] for s in st], [s['local'] for s in st] == slots)

        parent = slots.index(0)
        m.core(parent).call_once(L4.CALL % 'CheckShouldAdvanceLinkState')
        until(lambda: all(m.core(i).r8(m.sym['gReceivedRemoteLinkPlayers']) for i in range(N)), 120,
              'every game holding the others\' link players')
        by_slot = [ids[slots.index(s)] for s in range(N)]
        for i in range(N):
            seen = L4.players(m.core(i))
            check('game %d (slot %d) holds all four, in link order: %s' % (i, slots[i], seen), seen == by_slot)
        check('no link errors (%s)' % [L4.status(m.core(i))['errors'] for i in range(N)],
              not any(L4.status(m.core(i))['errors'] for i in range(N)))
        views = [view(b) for b in range(len(gbas))]
        for b, v in enumerate(views):
            print('    board %d: %s' % (b, {k: v[k] for k in ('transfers', 'frames', 'relayed', 'received',
                                                             'bad_checksums', 'dropped', 'lost', 'bad_packets')}))
        check('no relay saw a bad checksum from its GBAs', not any(v['bad_checksums'] for v in views))
        check('nothing dropped, lost or refused',
              not any(v['dropped'] or v['lost'] or v['bad_packets'] or v['out_lost'] for v in views))
        check('every board relayed its GBAs\' commands and got the others\'',
              all(v['relayed'] > 0 and v['received'] > 0 for v in views))
        for i in range(N):
            m.core(i).shot(os.path.join(out, 'link4-online-%s-%d.png' % (layout, i)))
    except RuntimeError as e:
        check(str(e), False)
        for b in socks:
            print('    board %d: %s' % (b, view(b)))
    finally:
        srv.kill()
        time.sleep(0.3)
    print('emerald link over Celio, boards %s  %s' % ('+'.join(map(str, gbas)),
                                                      'all checks passed' if not fails else 'FAILURES'))
    return 1 if fails else 0


LAYOUTS = ('1111', '31', '13', '22')   # 31 and 13: the three-GBA board standing in for the parent, and holding it

if __name__ == '__main__':
    rom = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('EMERALD_ROM', '')
    if not rom or not os.path.exists(rom):
        print('emerald link over Celio  skipped: no ROM')
        sys.exit(77)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(E.ROOT, 'build/emerald')
    layouts = sys.argv[3:] or LAYOUTS
    if len(layouts) == 1:
        sys.exit(main(rom, out, layouts[0]))
    rc = 0
    for lay in layouts:                        # each in its own process: the cores are per process
        rc |= subprocess.call([sys.executable, '-I', os.path.abspath(__file__), rom, out, lay])
    sys.exit(rc)
