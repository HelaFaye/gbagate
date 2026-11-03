"""rfu_union2.py -- two Pokemon Emeralds in the Union Room together, over our
Wireless Adapter emulation.

Each player is its own mGBA process (rfu_union.py --child) on its own
GB-Link simulator in Wireless Adapter mode (0x07), each joined to its
emulator by a shared-memory cable. Between the two adapters, the 64-byte
RFU1 stream relay test_rfu.py uses: what a host bridge, the netplay bridge
or Celio carries for mode 0x07.

    python3 -I test/mgba/rfu_union2.py <emerald.gba> <out-dir> [seconds]

Checked, each way: the scans find the other adapter; the room lists the
other trainer among its players; and its field shows them with the sprite
the game's own table gives their gender and trainer ID.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'gblink_sim'))


def main(rom, out, seconds):
    os.makedirs(out, exist_ok=True)
    import simdrv
    from test_rfu import enter_wireless, Relay
    tmp = tempfile.mkdtemp()
    wires = [os.path.join(tmp, 'a.wire'), os.path.join(tmp, 'b.wire')]
    sims = []
    for w in wires:
        os.environ['GBLINK_RFU_WIRE'] = w
        sims.append(simdrv.Sim())
    del os.environ['GBLINK_RFU_WIRE']
    fails = 0
    try:
        for s in sims:
            enter_wireless(s)
        relay = Relay(sims[0], sims[1])
        kids = [subprocess.Popen([sys.executable, '-I', os.path.join(HERE, 'rfu_union.py'), '--child', rom, w,
                                  str(delay), str(seconds), os.path.join(out, 'union2-%s.png' % tag)],
                                 stdout=subprocess.PIPE, text=True)
                for w, delay, tag in ((wires[0], 0, 'a'), (wires[1], 97, 'b'))]
        res = []
        for k in kids:
            o, _ = k.communicate(timeout=seconds + 240)
            lines = [l for l in o.splitlines() if l.startswith('{')]
            res.append(json.loads(lines[-1]) if lines else {'error': True, 'missing': o[-500:]})
        relay.close()
        chunks = [sum(1 for d in s.data if len(d) == 64) for s in sims]
        print('  radio chunks each adapter sent to the relay: %s' % chunks)
    finally:
        for s in sims:
            s.close()
    print('emerald union room, two players')
    for tag, r in zip('AB', res):
        print('  player %s: trainer %s, wire %s' % (tag, r.get('trainer_id'), r.get('wire')))
    ids = [r.get('trainer_id') for r in res]
    checks = [('two different trainers (%s)' % ids, ids[0] != ids[1]),
              ('neither game errors', not any(r.get('error') for r in res))]
    for me, other, a, b in ((res[0], res[1], 'A', 'B'), (res[1], res[0], 'B', 'A')):
        scans = me.get('scans', [0, 0])
        checks.append(('%s\'s scans found %s (%s of %s polls)' % (a, b, scans[1], scans[0]), scans[1] > 0))
        checks.append(('%s\'s Union Room lists %s as a player (%s)' % (a, b, me.get('leaders')),
                       other.get('trainer_id') in me.get('leaders', [])))
        checks.append(('%s\'s field shows %s with the sprite the game gives them (%s; seen %s)'
                       % (a, b, other.get('appearance'), me.get('field_gfx')),
                       other.get('appearance') in me.get('field_gfx', [])))
    for what, ok in checks:
        print('  %s  %s' % ('ok  ' if ok else 'FAIL', what))
        fails += 0 if ok else 1
    print('emerald union room, two players  %s' % ('all checks passed' if not fails else 'FAILURES'))
    return 1 if fails else 0


if __name__ == '__main__':
    rom = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('EMERALD_ROM', '')
    if not rom or not os.path.exists(rom):
        print('emerald union room, two players  skipped: no ROM')
        sys.exit(77)
    sys.exit(main(rom, sys.argv[2] if len(sys.argv) > 2 else 'build/emerald',
                  float(sys.argv[3]) if len(sys.argv) > 3 else 25))
