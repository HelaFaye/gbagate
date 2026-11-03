"""rfu_union.py -- Pokemon Emerald's Union Room with a Wireless Adapter: the
game on mGBA, the adapter our firmware's Wireless Adapter mode (0x07) in the
GB-Link simulator, the two joined by a shared-memory link cable
(test/gblink_sim/rfu_wire.h). The emulator is paced to real time, since the
adapter is firmware with real-time waits.

    python3 -I test/mgba/rfu_union.py <emerald.gba> <out-dir>

Without an adapter the room's RunUnionRoom ends in "Communication error"
(union_room.py shows it). With ours plugged in it must not: the game finds
the adapter, logs in, and the room runs.
"""
import ctypes
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'gblink_sim'))
import emerald as E          # noqa: E402
import union_room as U       # noqa: E402

fails = 0


def check(what, ok):
    global fails
    print('  %s  %s' % ('ok  ' if ok else 'FAIL', what))
    fails += 0 if ok else 1


def counters(e):
    c = (ctypes.c_uint32 * 8)()
    e.lib.t_rfu_counters(c)
    return dict(zip(('gba_words', 'adapter_words', 'unstaged', 'unarmed', 'clash', 'late_arm',
                     'enabled', 'late_us_max'), list(c)))


def main(rom, out):
    os.makedirs(out, exist_ok=True)
    wire = os.path.join(tempfile.mkdtemp(), 'rfu.wire')
    os.environ['GBLINK_RFU_WIRE'] = wire
    import simdrv            # noqa: E402  (after the env, which its Popen inherits)
    from test_rfu import enter_wireless   # noqa: E402

    print('emerald + adapter')
    e = E.Emu(rom)
    U.new_game(e)                              # at full speed, no adapter yet
    check('a new game reaches the overworld', e.name_of(e.callback2()) == 'CB2_Overworld')

    s = simdrv.Sim()
    try:
        enter_wireless(s)                      # SetMode 0x07: the adapter is up
        if e.lib.t_rfu_attach(wire.encode()) != 0:
            raise SystemExit('could not attach the wire')
        t0 = time.time()
        U.warp_in(e, quiet=False)              # the room's own scripts: RunUnionRoom
        check('the warp lands in the Union Room', e.location()[:2] == U.UNION_ROOM)
        seen = []
        for k in range(12):                    # ~12 s of game, in real time
            e.run(60)
            n = e.name_of(e.callback2())
            seen.append(n)
            if n == 'CB2_PrintErrorMessage':
                break
        c = counters(e)
        print('    %.1f s real for %d frames; wire %s' % (time.time() - t0, e.frame(), c))
        e.shot(os.path.join(out, 'rfu-union.png'))
        check('the game talked to the adapter (%d words it clocked, %d the adapter clocked)'
              % (c['gba_words'], c['adapter_words']), c['gba_words'] > 10)
        check('no communication error (%s)' % seen[-1], 'CB2_PrintErrorMessage' not in seen)
        check('RunUnionRoom is running (%s)' % ', '.join(U.tasks(e)), 'Task_RunUnionRoom' in U.tasks(e))
    finally:
        s.close() if hasattr(s, 'close') else s.p.kill()
    print('emerald + adapter  %s' % ('all checks passed' if not fails else 'FAILURES'))
    return 1 if fails else 0


def child(rom, wire, delay, seconds, shot):
    """One player: a new game (its timing set by `delay`, so trainer IDs
    differ), the adapter on `wire`, into the room, `seconds` there. Prints
    one JSON line: who this player is, how the others' rooms must draw them,
    and what this room saw."""
    import json
    e = E.Emu(rom)
    e.run(delay)
    U.new_game(e)
    if e.lib.t_rfu_attach(wire.encode()) != 0:
        raise SystemExit('could not attach the wire')
    U.warp_in(e, quiet=False)
    for _ in range(3):                         # off the doorway, into the room:
        e.run(16, E.UP)                        # the field drops objects out of view
        e.run(8)
    objs = e.sym['gObjectEvents']              # 16 x struct ObjectEvent (0x24)
    field_gfx, error = set(), False
    end = time.time() + seconds
    while time.time() < end:
        e.run(10)
        field_gfx |= {e.r8(objs + i * 0x24 + 5) for i in range(16) if e.r8(objs + i * 0x24) & 1}
        if e.name_of(e.callback2()) == 'CB2_PrintErrorMessage':
            error = True
            break
    e.shot(shot)
    # uRoom->playerList: struct RfuPlayer (32 bytes), the trainer ID at +2
    ur = e.r32(e.sym['sWirelessLinkMain'])
    pl = e.r32(ur) if 0x02000000 <= ur < 0x02040000 else 0
    leaders = [e.r16(pl + 32 * j + 2) for j in range(8)] if pl else []
    sb = e.r32(e.sym['gSaveBlock2Ptr'])
    gender, tid = e.r8(sb + 8), e.r16(sb + 0x0A)
    # how the others' rooms must draw this player: the game's own table,
    # sUnionRoomObjGfxIds[gender][trainerId[0] % NUM_UNION_ROOM_CLASSES]
    appearance = e.r8(e.sym['sUnionRoomObjGfxIds'] + gender * 10 + (tid & 0xFF) % 8)
    e.lib.t_rfu_scans.restype = ctypes.c_uint32
    e.lib.t_rfu_cmd_count.restype = ctypes.c_uint32
    cmds = {'%02x' % i: e.lib.t_rfu_cmd_count(i) for i in range(256) if e.lib.t_rfu_cmd_count(i)}
    print(json.dumps({'trainer_id': tid, 'appearance': appearance, 'field_gfx': sorted(field_gfx),
                      'leaders': [x for x in leaders if x], 'error': error, 'tasks': U.tasks(e),
                      'scans': [e.lib.t_rfu_scans(0), e.lib.t_rfu_scans(1)], 'cmds': cmds,
                      'wire': counters(e)}), flush=True)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--child':
        child(sys.argv[2], sys.argv[3], int(sys.argv[4]), float(sys.argv[5]), sys.argv[6])
        sys.exit(0)
    rom = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('EMERALD_ROM', '')
    if not rom or not os.path.exists(rom):
        print('emerald + adapter  skipped: no ROM (EMERALD_ROM or the first argument)')
        sys.exit(77)
    sys.exit(main(rom, sys.argv[2] if len(sys.argv) > 2 else os.path.join(E.ROOT, 'build/emerald')))
