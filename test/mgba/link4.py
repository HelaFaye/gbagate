"""link4.py -- four Pokemon Emeralds on one Multi-Player link cable, on mGBA
(mgba_multi.c's lockstep cores and cable): the ground truth for a 4-player
link, before any of it goes through our board.

    python3 -I test/mgba/link4.py <emerald.gba> <out-dir>

Each player is a new game (its own timing, so its own trainer ID), in the
overworld. Then, with the game's own link code:
  - every game calls OpenLink(), as the Cable Club does;
  - the cable runs: Multi-Player transfers, the parent's Start;
  - every game sees four players, and only the parent is master;
  - the parent calls CheckShouldAdvanceLinkState(), and the games exchange
    their link player records;
  - every game then holds all four players -- the same four trainer IDs,
    in link order, on all four.
"""
import ctypes
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emerald as E      # noqa: E402
import union_room as U   # noqa: E402

N = 4
DELAYS = (0, 97, 211, 307)          # frames before the new game: different trainer IDs

CALL = '''
typedef void (*fn)(void);
void entry(void) {
    ((fn)(SYM_%s | 1))();
    *(volatile unsigned *)(SYM_gMain + 4) = SYM_CB2_Overworld | 1;
    ((fn)(SYM_CB2_Overworld | 1))();
}
'''

fails = 0


def check(what, ok):
    global fails
    print('  %s  %s' % ('ok  ' if ok else 'FAIL', what))
    fails += 0 if ok else 1


def status(c):
    s = c.r32(c.sym['gLinkStatus'])
    return {'count': (s & 0x1C) >> 2, 'master': bool(s & 0x20), 'conn': bool(s & 0x40),
            'local': s & 3, 'errors': (s >> 12) & 0x7F}


def players(c):
    base = c.sym['gLinkPlayers']
    out = []
    for i in range(N):
        b = c.read(base + 28 * i, 28)
        out.append(struct.unpack_from('<I', b, 4)[0])
    return out


TOPOLOGIES = (
    ('one cable', None),                       # the four GBAs on one cable
    ('relayed, no delay', 0),                  # each on its own cable, mp_relay answering the rest
    ('relayed, 50 ms each way', 820),          # ... the relays' commands 820 quanta (~50 ms) late
)


def make_states(rom, out):
    """Each player's game, alone, to the overworld; cached in out."""
    states, ids = [], []
    for k, d in enumerate(DELAYS):
        path = os.path.join(out, 'link4-player%d.state' % k)
        if not os.path.exists(path):
            e = E.Emu(rom)
            e.run(d)
            U.new_game(e)
            open(path, 'wb').write(e.save_state())
        data = open(path, 'rb').read()
        e = E.Emu(rom)
        e.load_state(data)
        sb = e.r32(e.sym['gSaveBlock2Ptr'])
        ids.append(struct.unpack('<I', e.read(sb + 0x0A, 4))[0])
        states.append(data)
    return states, ids


def main(rom, out, name, delay):
    os.makedirs(out, exist_ok=True)
    print('emerald link, four players: %s' % name)
    states, ids = make_states(rom, out)
    check('four different trainers (%s)' % ids, len(set(ids)) == N)

    m = E.Multi(rom, N)
    for i, st in enumerate(states):
        m.core(i).load_state(st)
    if delay is not None:
        m.lib.m_relay_mode(delay)
    m.attach()
    m.run(30)
    for i in range(N):
        m.core(i).call_once(CALL % 'OpenLink')
    for _ in range(80):
        m.run(15)
        if all(status(m.core(i))['count'] == N for i in range(N)):
            break
    st = [status(m.core(i)) for i in range(N)]
    n, last = m.transfers()
    print('    after OpenLink: %s; %d cable transfers, last %s' % (st, n, ['%04x' % w for w in last]))
    check('the cable runs (%d transfers)' % n, n > 0)
    check('every game sees four players', all(s['count'] == N for s in st))
    check('only the parent is master', [s['master'] for s in st] == [True, False, False, False])
    check('each game knows its own link ID (%s)' % [s['local'] for s in st], [s['local'] for s in st] == list(range(N)))

    m.core(0).call_once(CALL % 'CheckShouldAdvanceLinkState')
    for _ in range(160):
        m.run(15)
        if all(m.core(i).r8(m.sym['gReceivedRemoteLinkPlayers']) for i in range(N)):
            break
    got = [m.core(i).r8(m.sym['gReceivedRemoteLinkPlayers']) for i in range(N)]
    check('every game received the others\' link players (%s)' % got, all(got))
    seen = [players(m.core(i)) for i in range(N)]
    for i in range(N):
        check('game %d holds all four, in link order: %s' % (i, seen[i]), seen[i] == ids)
    check('no link errors (%s)' % [status(m.core(i))['errors'] for i in range(N)],
          not any(status(m.core(i))['errors'] for i in range(N)))
    if delay is not None:
        stats = []
        for i in range(N):
            v = (ctypes.c_uint32 * 9)()
            m.lib.m_relay_stats(i, v)
            stats.append(dict(zip(('frames', 'relayed', 'received', 'idle', 'dropped', 'bad_checksums',
                                   'phase', 'connected', 'transfers'), list(v))))
        for i, st_ in enumerate(stats):
            print('    relay %d: %s' % (i, st_))
        check('no relay saw a bad checksum from its GBA', not any(x['bad_checksums'] for x in stats))
        check('every relay passed its GBA\'s commands on and got the others\'',
              all(x['relayed'] > 0 and x['received'] > 0 and not x['dropped'] for x in stats))
    tag = 'cable' if delay is None else 'relay%d' % delay
    for i in range(N):
        m.core(i).shot(os.path.join(out, 'link4-%s-%d.png' % (tag, i)))
    with open(os.path.join(out, 'link4-%s-trace.txt' % tag), 'w') as f:   # cable 0, word by word
        m.lib.m_trace_quantum.restype = ctypes.c_uint32
        for k, w in enumerate(m.trace()):
            f.write('%5d  q%-7d %s\n' % (k, m.lib.m_trace_quantum(k), ' '.join('%04x' % x for x in w)))
    print("emerald link, four players: %s  %s" % (name, "all checks passed" if not fails else "FAILURES"))
    return 1 if fails else 0


if __name__ == '__main__':
    rom = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('EMERALD_ROM', '')
    if not rom or not os.path.exists(rom):
        print('emerald link, four players  skipped: no ROM')
        sys.exit(77)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(E.ROOT, 'build/emerald')
    if len(sys.argv) > 3:                      # one topology, in this process
        k = int(sys.argv[3])
        sys.exit(main(rom, out, *TOPOLOGIES[k]))
    import subprocess                          # each in its own: the cable is per process
    rc = 0
    for k in range(len(TOPOLOGIES)):
        rc |= subprocess.call([sys.executable, '-I', os.path.abspath(__file__), rom, out, str(k)])
    sys.exit(rc)
