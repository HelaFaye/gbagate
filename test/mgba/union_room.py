"""union_room.py -- Pokemon Emerald on mGBA: into the Union Room, and the first
way of making it ours.

    python3 -I test/mgba/union_room.py <emerald.gba> <out-dir>

Steps, each checked, each leaving a screenshot and a state in <out-dir>:

1. A new game, played from power-on by pressing buttons, to the overworld
   (the moving truck).
2. Into the Union Room with the game's own warp: an injected Thumb routine
   runs once as gMain.callback2, calls SetWarpDestination(MAP_UNION_ROOM,
   warp 0) and DoWarp(), and hands back to CB2_Overworld.
3. With no Wireless Adapter the room's on-resume script (special
   RunUnionRoom) fails to "Communication error". That is checked, then:
4. The same entry with the room's scripts redirected. gMapHeader is the RAM
   copy of the map header; the game copies it from ROM and runs the room's
   scripts from it. Pointing its mapScripts at an empty table on the frame of
   the copy, before ResumeMap runs them, enters a quiet room: no
   RunUnionRoom task, no error, the overworld running.

Needs build/libmgba_shim.so (make mgba-shim) and pret's symbols
(test/mgba/fetch_symbols.sh). The ROM must be the one pokeemerald rebuilds.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emerald as E   # noqa: E402

UNION_ROOM = (25, 60)          # from pokeemerald's data/maps/map_groups.json

WARP = '''
typedef void (*fn)(void);
typedef void (*warp_fn)(int, int, int, int, int);
void entry(void) {
    ((warp_fn)(SYM_SetWarpDestination | 1))(MAP_GROUP, MAP_NUM, WARP_ID, -1, -1);
    ((fn)(SYM_DoWarp | 1))();
    *(volatile unsigned *)(SYM_gMain + 4) = SYM_CB2_Overworld | 1;
    ((fn)(SYM_CB2_Overworld | 1))();
}
'''

fails = 0


def check(what, ok):
    global fails
    print('  %s  %s' % ('ok  ' if ok else 'FAIL', what))
    fails += 0 if ok else 1


def new_game(e):
    for i in range(3000):
        n = e.name_of(e.callback2())
        if n == 'CB2_Overworld':
            break
        if 'Naming' in n:
            e.press(E.START)
            e.press(E.A)
        else:
            e.press(E.A if i % 3 else E.START, hold=3, after=9)
    e.run(240)


def tasks(e):
    T = e.sym['gTasks']
    return [e.name_of(e.r32(T + i * 40)) for i in range(16) if e.r8(T + i * 40 + 4)]


def warp_in(e, quiet):
    e.call_once(WARP, defines={'MAP_GROUP': UNION_ROOM[0], 'MAP_NUM': UNION_ROOM[1], 'WARP_ID': 0})
    mh, scripts = e.sym['gMapHeader'], e.sym['UnionRoom_MapScripts']
    empty = E.FREE_EWRAM + 0x800
    e.w32(empty, 0)
    for _ in range(300):
        e.run(1)
        if quiet and e.r32(mh + 8) == scripts:
            e.w32(mh + 8, empty)


def main(rom, out):
    os.makedirs(out, exist_ok=True)
    e = E.Emu(rom)
    print('emerald union room')
    new_game(e)
    check('a new game reaches the overworld (%s, map %s)' % (e.name_of(e.callback2()), e.location()[:2]),
          e.name_of(e.callback2()) == 'CB2_Overworld')
    e.shot(os.path.join(out, '1-overworld.png'))
    start = e.save_state()

    warp_in(e, quiet=False)
    check('the warp lands in the Union Room %s' % (e.location()[:2],), e.location()[:2] == UNION_ROOM)
    e.run(300)
    check('without an adapter, RunUnionRoom ends in the link error (%s)' % e.name_of(e.callback2()),
          e.name_of(e.callback2()) == 'CB2_PrintErrorMessage')
    e.shot(os.path.join(out, '2-no-adapter.png'))

    e.load_state(start)
    warp_in(e, quiet=True)
    e.run(300)
    check('scripts redirected: in the room, overworld running (%s, %s)' % (e.name_of(e.callback2()), e.location()[:2]),
          e.name_of(e.callback2()) == 'CB2_Overworld' and e.location()[:2] == UNION_ROOM)
    check('... and no RunUnionRoom task (%s)' % ', '.join(tasks(e)), 'Task_RunUnionRoom' not in tasks(e))
    e.shot(os.path.join(out, '3-quiet-room.png'))
    open(os.path.join(out, 'union_quiet.state'), 'wb').write(e.save_state())
    print('emerald union room  %s' % ('all checks passed' if not fails else 'FAILURES'))
    return 1 if fails else 0


if __name__ == '__main__':
    rom = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('EMERALD_ROM', '')
    if not rom or not os.path.exists(rom):
        print('emerald union room  skipped: no ROM (EMERALD_ROM or the first argument)')
        sys.exit(77)
    sys.exit(main(rom, sys.argv[2] if len(sys.argv) > 2 else os.path.join(E.ROOT, 'build/emerald')))
