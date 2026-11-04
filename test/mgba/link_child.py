"""link_child.py -- one Pokemon Emerald on a Multi-Player cable to a simulated
board (test/gblink_sim with GBLINK_MP_WIRE, test/gblink_sim/mp_wire.h),
running the game's own link code, for celio_emerald.mjs.

    python3 -I test/mgba/link_child.py <rom> <wire> <delay> <seconds> <shot>

A new game (its timing from `delay`, so its own trainer ID), the cable
plugged in, then as the Cable Club does it: OpenLink(); once the game counts
two players, the parent (master) calls CheckShouldAdvanceLinkState(); the
games exchange link player records. Prints one JSON line.
"""
import ctypes
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import emerald as E      # noqa: E402
import union_room as U   # noqa: E402

CALL = '''
typedef void (*fn)(void);
void entry(void) {
    ((fn)(SYM_%s | 1))();
    *(volatile unsigned *)(SYM_gMain + 4) = SYM_CB2_Overworld | 1;
    ((fn)(SYM_CB2_Overworld | 1))();
}
'''


def status(e):
    s = e.r32(e.sym['gLinkStatus'])
    return {'count': (s & 0x1C) >> 2, 'master': bool(s & 0x20), 'local': s & 3, 'errors': (s >> 12) & 0x7F}


def main(rom, wire, delay, seconds, shot):
    e = E.Emu(rom)
    e.run(delay)
    U.new_game(e)
    sb = e.r32(e.sym['gSaveBlock2Ptr'])
    tid = struct.unpack('<I', e.read(sb + 0x0A, 4))[0]
    if e.lib.t_mp_attach(wire.encode()) != 0:
        raise SystemExit('could not attach the cable')
    print(json.dumps({'ready': True, 'trainer_id': tid}), flush=True)
    e.run(30)
    e.call_once(CALL % 'OpenLink')
    advanced, got, t_count, t_done = False, False, None, None
    t0 = time.time()
    end = t0 + seconds
    while time.time() < end:
        e.run(15)
        st = status(e)
        if st['count'] == 2 and t_count is None:
            t_count = time.time() - t0
        if st['count'] == 2 and st['master'] and not advanced:
            e.call_once(CALL % 'CheckShouldAdvanceLinkState')
            advanced = True
        if e.r8(e.sym['gReceivedRemoteLinkPlayers']):
            got = True
            t_done = time.time() - t0
            e.run(30)
            break
    base = e.sym['gLinkPlayers']
    players = [struct.unpack_from('<I', e.read(base + 28 * i, 28), 4)[0] for i in range(2)]
    c = (ctypes.c_uint32 * 4)()
    e.lib.t_mp_counters(c)
    e.shot(shot)
    print(json.dumps({'trainer_id': tid, 'status': status(e), 'received': got, 'players': players,
                      'count_s': t_count, 'done_s': t_done,
                      'cable': dict(zip(('transfers', 'unanswered', 'not_multi', 'late_us_max'), list(c)))}),
          flush=True)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4]), sys.argv[5])
