"""crowd_capacity.py -- how many other players Emerald can show at once, two ways.

    python3 -I test/mgba/crowd_capacity.py <emerald.gba> <out-dir>

doc/MMO.md's player limit is the game's: other players are object events,
16 slots shared with the player and the map's NPCs. gba-mmo
(github.com/GB-Link/gba-mmo) draws other players differently: as plain
sprites ("world objects") that the game does not otherwise know about. This
measures both, in a real Emerald, in Littleroot Town quieted as
overworld_sync.py quiets it (no townspeople):

  1. object events: SpawnSpecialObjectEventParameterized with Brendan's
     graphics, one per tile, until the game says no (it returns
     OBJECT_EVENTS_COUNT) -- figures that walk, face, collide and can be
     talked to;
  2. sprite-only figures: CreateObjectGraphicsSprite with the same graphics,
     on top, until it fails (MAX_SPRITES, or no room for their tiles) --
     figures the game draws but does not move, collide or script;

then checks that the game keeps running (frames counted, the player still
answers the D-pad), that the hardware draws them (OBJ entries in OAM), and
that they are on screen. Saves a screenshot of each stage.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import emerald as E          # noqa: E402
import link4 as L4           # noqa: E402
import overworld_sync as O   # noqa: E402

# This test has no agent stand-in and no mailbox, so their rooms
# (overworld_sync.py's map of the free EWRAM) hold the snippets and results.
CODE = E.FREE_EWRAM + 0x900
SCRATCH = E.FREE_EWRAM + 0xD00            # results of the C snippets below

SPAWN_ALL = '''
typedef unsigned char u8;
typedef short s16;
typedef u8 (*spawn_fn)(u8, u8, u8, s16, s16, u8);
void entry(void) {
    volatile unsigned short *out = (volatile unsigned short *)SCRATCH;
    int n = 0;
    for (int k = 0; k < 24; k++) {
        /* a tile each, around the player's spot (map coordinates, +7 the border) */
        const s16 x = (s16)(4 + 7 + (k % 8) * 2), y = (s16)(9 + 7 + (k / 8) * 2);
        const u8 id = ((spawn_fn)(SYM_SpawnSpecialObjectEventParameterized | 1))(GFX, 0, (u8)(0x40 + k), x, y, 3);
        if (id >= 16) break;
        n++;
    }
    out[0] = (unsigned short)n;
    /* call_once runs this as gMain.callback2: give the frame back to the overworld */
    *(volatile unsigned *)(SYM_gMain + 4) = SYM_CB2_Overworld | 1;
    ((void (*)(void))(SYM_CB2_Overworld | 1))();
}
'''

SPRITES = '''
typedef unsigned char u8;
typedef short s16;
typedef u8 (*create_fn)(unsigned short, void (*)(void *), s16, s16, u8);
void entry(void) {
    volatile unsigned short *out = (volatile unsigned short *)SCRATCH;
    int n = 0;
    for (int k = 0; k < 80; k++) {
        const s16 x = (s16)(12 + (k % 10) * 24), y = (s16)(20 + (k / 10) * 20);
        const u8 id = ((create_fn)(SYM_CreateObjectGraphicsSprite | 1))(GFX, (void (*)(void *))(SYM_SpriteCallbackDummy | 1), x, y, 0);
        if (id >= 64) break;
        /* in the world, not on the glass: the camera's scroll applies to it
         * (struct Sprite's coordOffsetEnabled, bit 1 at +0x3E) */
        *(volatile u8 *)(SYM_gSprites + 0x44 * id + 0x3E) |= 2;
        out[2 + n] = id;
        n++;
    }
    out[1] = (unsigned short)n;
    /* call_once runs this as gMain.callback2: give the frame back to the overworld */
    *(volatile unsigned *)(SYM_gMain + 4) = SYM_CB2_Overworld | 1;
    ((void (*)(void))(SYM_CB2_Overworld | 1))();
}
'''

fails = 0


def check(what, ok):
    global fails
    print('  %s  %s' % ('ok  ' if ok else 'FAIL', what))
    fails += 0 if ok else 1


def sprites_in_use(c):
    """gSprites' slots in use (struct Sprite is 0x44 bytes; inUse is bit 0 at +0x3E)."""
    g = c.sym['gSprites']
    return sum(1 for k in range(64) if c.r8(g + 0x44 * k + 0x3E) & 1)


def oam_objects(c):
    """OBJ entries the hardware draws: OAM attribute 0's disable bit clear
    (bit 9 without the affine bit 8), on screen vertically."""
    n = 0
    for k in range(128):
        a0 = c.r16(0x07000000 + 8 * k)
        if (a0 & 0x300) == 0x200:
            continue
        y = a0 & 0xFF
        if y < 160 or y >= 224:            # 224..255 wraps to the top
            n += 1
    return n


def finish(m, c, limit=600):
    """Run until the snippet has given callback2 back to the overworld: making
    dozens of sprites takes the game more than a frame."""
    for _ in range(limit):
        m.run(1)
        if c.r32(c.sym['gMain'] + 4) == c.sym['CB2_Overworld'] | 1:
            return True
    return False


def frames(c):
    return c.r32(c.sym['gMain'] + 0x20)    # gMain.vblankCounter1


def main(rom, out):
    os.makedirs(out, exist_ok=True)
    print('emerald: how many other players on screen, object events and sprite-only figures')
    states, _ = L4.make_states(rom, out)
    m = E.Multi(rom, 1)
    m.core(0).load_state(states[0])
    g = O.Game(m, 0)
    c = g.c
    m.run(10)
    where = O.warp_all(m, [g], spots=((10, 13),))
    check('in Littleroot Town, quieted (%s)' % (where,), where[0][:2] == O.LITTLEROOT)
    m.run(30)
    before = g.player()[2:4]
    c.press(0x80, hold=16, after=16)                     # down
    walked = g.player()[2:4]
    check('the player walks before the crowd (%s -> %s)' % (before, walked), before != walked)
    base_sprites = sprites_in_use(c)
    base_oam = oam_objects(c)
    print('    before: %d sprites in use, %d OBJ entries drawn' % (base_sprites, base_oam))
    c.shot(os.path.join(out, 'crowd-0-empty.png'))

    # 1. object events
    c.w16(SCRATCH, 0)
    c.call_once(SPAWN_ALL, base=CODE, defines={'GFX': O.GFX_BRENDAN, 'SCRATCH': hex(SCRATCH)})
    check('the object events are made, and the overworld runs again', finish(m, c))
    m.run(20)
    n_obj = c.r16(SCRATCH)
    used = sum(1 for k in range(16) if c.r8(c.sym['gObjectEvents'] + O.OBJ * k) & 1)
    print('    object events: %d other players spawned; %d of 16 slots in use (the player holds one)' % (n_obj, used))
    check('object events: the slots fill (%d players), and no more are given' % n_obj, n_obj >= 1 and used == 16)
    c.shot(os.path.join(out, 'crowd-1-object-events.png'))
    obj_sprites = sprites_in_use(c)

    # 2. sprite-only figures on top
    c.w16(SCRATCH + 2, 0)
    c.call_once(SPRITES, base=CODE, defines={'GFX': O.GFX_BRENDAN, 'SCRATCH': hex(SCRATCH)})
    check('the sprite-only figures are made, and the overworld runs again', finish(m, c))
    m.run(2)
    n_spr = c.r16(SCRATCH + 2)
    ids = [c.r16(SCRATCH + 4 + 2 * k) for k in range(n_spr)]
    total = sprites_in_use(c)
    print('    sprite-only figures: %d more (sprites in use %d -> %d of 64)' % (n_spr, obj_sprites, total))
    check('sprite-only figures add players beyond the object events (%d)' % n_spr, n_spr > 0)

    f0 = frames(c)
    m.run(60)
    f1 = frames(c)
    oam = oam_objects(c)
    print('    after 60 frames: the game counted %d; %d OBJ entries drawn (gOamLimit %d)' % (
        f1 - f0, oam, c.r8(c.sym['gOamLimit'])))
    check('the game keeps running (60 frames in 60)', f1 - f0 == 60)
    g_spr = c.sym['gSprites']
    ox, oy = struct.unpack('<hh', c.read(c.sym['gSpriteCoordOffsetX'], 4))

    def screen_pos(i):
        """Where the sprite system draws sprite i: x + x2 (+ the camera's
        offset, for a sprite in the world), its centre."""
        x, y, x2, y2 = struct.unpack('<hhhh', c.read(g_spr + 0x44 * i + 0x20, 8))
        flags = c.r8(g_spr + 0x44 * i + 0x3E)
        if flags & 2:
            x, y = x + ox, y + oy
        return x + x2, y + y2, bool(flags & 4)

    shown = 0
    for i in ids:
        x, y, inv = screen_pos(i)
        shown += (-8 <= x < 248 and -16 <= y < 176 and not inv)
    print('    sprite-only figures within the screen, where the camera puts them: %d of %d' % (shown, n_spr))
    check('the hardware draws every figure on screen: OBJ entries %d >= the player, %d object events and %d figures'
          % (oam, n_obj, shown), oam >= 1 + n_obj + shown)
    check('most figures are on screen (%d of %d)' % (shown, n_spr), shown >= n_spr * 3 // 4)
    c.shot(os.path.join(out, 'crowd-2-sprites.png'))

    # the player still walks among them, and the figures stay where they
    # stand in the world: their drawn position moves with the camera
    def drawn_y(i):
        return c.r8(g_spr + 0x44 * i)                 # oam.y, as the sprite system last built it
    probe = ids[0]
    y0 = drawn_y(probe)
    before = g.player()[2:4]
    for _ in range(3):
        c.press(0x80, hold=16, after=8)               # down
    m.run(20)
    after = g.player()[2:4]
    check('the player still answers the D-pad (%s -> %s)' % (before, after), before != after)
    moved = (after[1] - before[1]) * 16
    dy = (y0 - drawn_y(probe)) & 0xFF
    check('a sprite-only figure stays put in the world: drawn %d px higher after %d px of walking' % (dy, moved),
          dy == moved)
    c.shot(os.path.join(out, 'crowd-3-walked.png'))

    print('    capacity here: %d as object events + %d sprite-only = %d other players on one screen'
          % (n_obj, n_spr, n_obj + n_spr))
    print('crowd capacity: %s' % ('all checks passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
