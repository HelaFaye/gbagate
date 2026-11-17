"""savetool_emerald.py -- the Save Tool blob (rom/savetool.bin) against a real
cartridge's flash: Pokemon Emerald's 128 KB flash, as mGBA emulates the chip.

    python3 -I test/mgba/savetool_emerald.py <emerald.gba> rom/savetool.bin

On the board, the GBA runs only the memory agent and this blob (both
multibooted), so all of EWRAM is theirs. Here the cartridge is Emerald, which
boots and uses EWRAM itself; so once it is at the title screen a small
monitor takes the CPU over for good -- interrupts off, a loop in the 4 KB of
EWRAM Emerald leaves free -- and runs the blob whenever this script raises
its flag, as the agent's CALL would. Then:

  - DETECT, in 4 MB spans as the board does it, finds FLASH1M_V: 128 KB flash;
  - FLASH_ID reads the chip's maker and device (mGBA's emulated chip);
  - with a known pattern put in the flash, READ brings all 128 KB, both
    banks, into the buffer, byte for byte;
  - WRITE, sector by sector, puts a second pattern in (0xFF runs, which the
    writer skips, included), each sector verified on the GBA; and mGBA's own
    copy of the flash then matches it exactly.

No save file is read or written: the flash is mGBA's in memory.
"""
import ctypes
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import emerald as E       # noqa: E402

BASE, BUF, SECTOR = 0x02001000, 0x02010000, 4096
SIZE = 0x20000
MBOX = E.FREE_EWRAM + 0x800              # go flag and a counter
CMD_DETECT, CMD_ID, CMD_READ, CMD_WRITE = 1, 2, 3, 4
TYPES = {0: 'none', 1: 'SRAM', 2: 'flash 64 KB', 3: 'flash 128 KB', 4: 'EEPROM'}

MONITOR = '''
typedef void (*fn)(void);
void entry(void) {
    *(volatile unsigned short *)0x04000208 = 0;          /* IME off, for good */
    volatile unsigned *mb = (volatile unsigned *)MBOX;
    for (;;) {
        if (mb[0]) {
            ((fn)(ENTRY))();
            mb[1]++;
            mb[0] = 0;
        }
    }
}
'''

fails = 0


def check(what, ok):
    global fails
    print('  %s  %s' % ('ok  ' if ok else 'FAIL', what))
    fails += 0 if ok else 1


class Blob:
    """The control block (rom/savetool.h), driven as the board drives it."""
    FIELDS = ('magic', 'version', 'entry', 'done', 'cmd', 'arg0', 'arg1', 'status',
              'save_type', 'save_size', 'found_at', 'chip_id', 'mismatches', 'buf', 'chip_size')

    def __init__(self, emu):
        self.e = emu

    def block(self):
        return dict(zip(self.FIELDS, struct.unpack('<15I', self.e.read(BASE, 60))))

    def run(self, cmd, arg0=0, arg1=0, frames=600):
        e = self.e
        e.w32(BASE + 16, cmd)
        e.w32(BASE + 20, arg0)
        e.w32(BASE + 24, arg1)
        e.w32(MBOX, 1)
        for _ in range(frames):
            e.run(1)
            if e.r32(MBOX) == 0:
                return self.block()
        raise RuntimeError('the blob did not finish command %d' % cmd)


def savedata(e):
    buf = (ctypes.c_uint8 * SIZE)()
    n = e.lib.t_savedata_get(buf, SIZE)
    return n, bytes(buf)


def put_savedata(e, data):
    return e.lib.t_savedata_put(bytes(data), len(data))


def main(rom, blob_path):
    print('savetool: a 128 KB flash save, read and written by the blob (mGBA, Emerald)')
    e = E.Emu(rom)
    e.lib.t_savedata_get.restype = ctypes.c_size_t
    e.lib.t_savedata_get.argtypes = (ctypes.c_void_p, ctypes.c_size_t)
    e.lib.t_savedata_put.argtypes = (ctypes.c_char_p, ctypes.c_size_t)
    e.run(400)                                           # the title screen: done with its own save
    n, _ = savedata(e)
    check('mGBA gives the cartridge 128 KB of save memory (%d)' % n, n == SIZE)

    rnd = random.Random(3)
    first = bytes(rnd.getrandbits(8) for _ in range(SIZE))
    check('a known pattern put in the flash', put_savedata(e, first) == 0 and savedata(e)[1] == first)

    blob = open(blob_path, 'rb').read()
    e.write(BASE, blob)
    b = Blob(e)
    head = b.block()
    check('the blob is in place (magic SVT1, entry %#x)' % head['entry'],
          head['magic'] == 0x31545653 and head['entry'] & 1)
    e.w32(MBOX, 0)
    e.w32(MBOX + 4, 0)
    prev = e.r32(e.sym['gMain'] + 4)
    e.call_once(MONITOR, defines={'MBOX': hex(MBOX), 'ENTRY': hex(head['entry'])})
    e.run(2)
    check('the monitor has the CPU (Emerald\'s callback2 no longer runs: was %s)' % e.name_of(prev),
          e.r32(e.sym['gMain'] + 4) != prev)

    # DETECT in 4 MB spans, as the board does
    found = None
    for start in range(0, 0x2000000, 0x400000):
        r = b.run(CMD_DETECT, start, 0x400000)
        if r['save_type']:
            found = r
            break
    check('DETECT finds the save library: %s at ROM %#x' % (
        TYPES.get(found['save_type']) if found else 'nothing', found['found_at'] if found else 0),
        found is not None and found['save_type'] == 3 and found['save_size'] == SIZE)
    sig = e.read(0x08000000 + found['found_at'], 9) if found else b''
    check('... the string there is FLASH1M_V (%r)' % sig, sig == b'FLASH1M_V')

    r = b.run(CMD_ID)
    maker, dev = r['chip_id'] & 0xFF, r['chip_id'] >> 8
    print('    flash chip: maker %02X device %02X' % (maker, dev))
    check('FLASH_ID reads a chip (not open bus or zero)', r['chip_id'] not in (0, 0xFFFF))
    check('... one we know: 128 KB, as the string says (chip_size %#x)' % r['chip_size'],
          r['chip_size'] == SIZE and r['save_type'] == 3)

    # The chip decides: a game whose string says FLASH512 on a 128 KB part
    # is read as 128 KB once the chip is known.
    e.w32(BASE + 32, 2)                                  # save_type: flash 64 KB, as such a string would set
    e.w32(BASE + 36, 0x10000)
    r = b.run(CMD_ID)
    check('a 64 KB string on this 128 KB chip: the chip wins (%s, %#x)' % (TYPES.get(r['save_type']), r['save_size']),
          r['save_type'] == 3 and r['save_size'] == SIZE)

    r = b.run(CMD_READ)
    got = e.read(BUF, SIZE)
    diff = sum(1 for x, y in zip(got, first) if x != y)
    check('READ: all 128 KB, both banks, byte for byte (%d differ; status %d)' % (diff, r['status']),
          r['status'] == 0 and got == first)

    second = bytearray(rnd.getrandbits(8) for _ in range(SIZE))
    for k in range(0, SIZE, 997):                        # erased runs: the writer skips 0xFF
        second[k:k + 40] = b'\xff' * 40
    second = bytes(second)
    e.write(BUF, second)
    statuses = []
    for s in range(SIZE // SECTOR):
        r = b.run(CMD_WRITE, s)
        statuses.append((r['status'], r['mismatches']))
    bad = [(s, st) for s, st in enumerate(statuses) if st != (0, 0)]
    check('WRITE: 32 sectors, each verified on the GBA (%d failed: %s)' % (len(bad), bad[:4]), not bad)
    n, now = savedata(e)
    diff = sum(1 for x, y in zip(now, second) if x != y)
    check('mGBA\'s flash now holds exactly what was written (%d bytes differ)' % diff, now == second)

    r = b.run(CMD_READ)
    check('and READ brings it back', e.read(BUF, SIZE) == second)
    print('savetool: %s' % ('all checks passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
