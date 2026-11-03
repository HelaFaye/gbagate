"""emerald.py -- Pokemon Emerald (US/EU, the build pret/pokeemerald matches
byte for byte) on mGBA, for the Union Room and memory-agent experiments.

Addresses come from pret's own symbol file (pokeemerald's `symbols` branch,
fetched into build/ref by fetch_symbols.sh), never typed by hand. The ROM is
the user's: its path comes from EMERALD_ROM or the command line, and its
SHA-1 must be the one pokeemerald builds, or the symbols mean nothing.
"""
import ctypes
import hashlib
import os
import re
import struct
import subprocess
import tempfile
import zlib

EMERALD_SHA1 = 'f3ae088181bf583e55daf962a92bb46f4f1d07b7'   # pokeemerald's rom.sha1

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# GBA keys, as mGBA's setKeys takes them
A, B, SELECT, START, RIGHT, LEFT, UP, DOWN, R, L = (1 << i for i in range(10))


class Symbols:
    def __init__(self, path):
        self.addr, self.size = {}, {}
        for line in open(path):
            p = line.split()
            if len(p) == 4:
                self.addr[p[3]] = int(p[0], 16)
                self.size[p[3]] = int(p[2], 16)

    def __getitem__(self, name):
        return self.addr[name]


def find_symbols():
    for p in (os.environ.get('POKEEMERALD_SYM', ''),
              os.path.join(ROOT, 'build/ref/pokeemerald.sym')):
        if p and os.path.exists(p):
            return Symbols(p)
    raise SystemExit('pokeemerald.sym not found: run test/mgba/fetch_symbols.sh')


def check_rom(path):
    h = hashlib.sha1(open(path, 'rb').read()).hexdigest()
    if h != EMERALD_SHA1:
        raise SystemExit('%s is not the ROM pokeemerald builds (sha1 %s)' % (path, h))


def write_png(path, rgb, w=240, h=160):
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


FREE_EWRAM = 0x0203F000   # past pokeemerald's last EWRAM symbol (0x0203CF64)


def build_thumb(src, base, sym, defines=None):
    """Compile src (C, entry point `entry`) as Thumb at `base`, with every
    SYM_<name> it uses defined from pret's symbols. Returns the bytes."""
    d = dict(defines or {})
    for name in set(re.findall(r'SYM_(\w+)', src)):
        d['SYM_' + name] = hex(sym[name])
    with tempfile.TemporaryDirectory() as t:
        c, o, elf, bin_ = (os.path.join(t, n) for n in ('x.c', 'x.o', 'x.elf', 'x.bin'))
        open(c, 'w').write(src)
        subprocess.run(['clang', '--target=arm-none-eabi', '-mcpu=arm7tdmi', '-mthumb', '-O2',
                        '-ffreestanding', '-fno-builtin', '-c', c, '-o', o] +
                       ['-D%s=%s' % kv for kv in d.items()], check=True)
        subprocess.run(['ld.lld', '-Ttext=%#x' % base, '-e', 'entry', o, '-o', elf], check=True)
        subprocess.run(['llvm-objcopy', '-O', 'binary', '-j', '.text', '-j', '.rodata', '-j', '.data',
                        elf, bin_], check=True)
        return open(bin_, 'rb').read()


class Emu:
    def __init__(self, rom, lib=os.path.join(ROOT, 'build/libmgba_shim.so')):
        check_rom(rom)
        self.lib = L = ctypes.CDLL(lib)
        for f in ('t_r8', 't_r16', 't_r32', 't_frame'):
            getattr(L, f).restype = ctypes.c_uint32
        L.t_state_size.restype = ctypes.c_size_t
        if L.t_open(rom.encode()) != 0:
            raise SystemExit('mGBA could not load the ROM')
        self.sym = find_symbols()

    # -- time and input -------------------------------------------------
    def run(self, frames, keys=0):
        self.lib.t_run(frames, keys)

    def press(self, keys, hold=4, after=12):
        self.run(hold, keys)
        self.run(after, 0)

    def frame(self):
        return self.lib.t_frame()

    # -- memory ---------------------------------------------------------
    def r8(self, a):  return self.lib.t_r8(a)
    def r16(self, a): return self.lib.t_r16(a)
    def r32(self, a): return self.lib.t_r32(a)
    def w8(self, a, v):  self.lib.t_w8(a, v & 0xFF)
    def w16(self, a, v): self.lib.t_w16(a, v & 0xFFFF)
    def w32(self, a, v): self.lib.t_w32(a, v & 0xFFFFFFFF)

    def read(self, a, n):
        return bytes(self.r8(a + i) for i in range(n))

    def write(self, a, data):
        for i, b in enumerate(data):
            self.w8(a + i, b)

    # -- the screen and states ------------------------------------------
    def screen(self):
        buf = (ctypes.c_uint8 * (240 * 160 * 3))()
        self.lib.t_screen(buf)
        return bytes(buf)

    def shot(self, path):
        write_png(path, self.screen())

    def save_state(self):
        n = self.lib.t_state_size()
        buf = (ctypes.c_uint8 * n)()
        if self.lib.t_save_state(buf) != 0:
            raise RuntimeError('saveState failed')
        return bytes(buf)

    def load_state(self, data):
        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        if self.lib.t_load_state(buf) != 0:
            raise RuntimeError('loadState failed')

    # -- the game -------------------------------------------------------
    def callback2(self):
        """gMain.callback2: which screen the game is on."""
        return self.r32(self.sym['gMain'] + 4)

    def inject(self, src, base=FREE_EWRAM, defines=None):
        """Build src at base and write it there. Returns its Thumb entry."""
        code = build_thumb(src, base, self.sym, defines)
        self.write(base, code)
        return base | 1

    def call_once(self, src, base=FREE_EWRAM, defines=None):
        """Run src once, from the main loop, as the game's callback2 for one
        frame. src must put callback2 back itself."""
        entry = self.inject(src, base, defines)
        self.w32(self.sym['gMain'] + 4, entry)

    def location(self):
        sb = self.r32(self.sym['gSaveBlock1Ptr'])
        return (self.r8(sb + 4), self.r8(sb + 5), struct.unpack('<h', self.read(sb, 2))[0],
                struct.unpack('<h', self.read(sb + 2, 2))[0])

    def name_of(self, addr):
        """The function symbol at addr (Thumb bit ignored)."""
        a = addr & ~1
        for n, v in self.sym.addr.items():
            if v == a and v >= 0x08000000:
                return n
        return hex(addr)


class Multi:
    """Up to four Emeralds on one Multi-Player link cable (mgba_multi.c),
    in lockstep. multi.core(i) is an Emu for core i: every method Emu has
    (symbols, memory, inject, call_once, ...) works on it."""

    def __init__(self, rom, n, lib=os.path.join(ROOT, 'build/libmgba_multi.so')):
        check_rom(rom)
        self.lib = L = ctypes.CDLL(lib)
        for f in ('m_r8', 'm_r16', 'm_r32', 'm_frame', 'm_bus_transfers'):
            getattr(L, f).restype = ctypes.c_uint32
        L.m_state_size.restype = ctypes.c_size_t
        if L.m_open(n, rom.encode()) != 0:
            raise SystemExit('mGBA could not load the ROM')
        self.n = n
        self.sym = find_symbols()
        self.cores = [_Core(self, i) for i in range(n)]

    def core(self, i):
        return self.cores[i]

    def attach(self):
        self.lib.m_bus_attach()

    def run(self, frames, keys=None):
        for i, k in enumerate(keys or [0] * self.n):
            self.lib.m_keys(i, k)
        self.lib.m_run(frames)

    def transfers(self):
        last = (ctypes.c_uint16 * 4)()
        n = self.lib.m_bus_transfers(last)
        return n, list(last)


class _Core(Emu):
    def __init__(self, multi, i):          # not Emu.__init__: the cores are multi's
        self.m, self.i, self.sym = multi, i, multi.sym

    def run(self, frames, keys=0):
        ks = [0] * self.m.n
        ks[self.i] = keys
        self.m.run(frames, ks)

    def frame(self): return self.m.lib.m_frame(self.i)
    def r8(self, a):  return self.m.lib.m_r8(self.i, a)
    def r16(self, a): return self.m.lib.m_r16(self.i, a)
    def r32(self, a): return self.m.lib.m_r32(self.i, a)
    def w8(self, a, v):  self.m.lib.m_w8(self.i, a, v & 0xFF)
    def w16(self, a, v): self.m.lib.m_w16(self.i, a, v & 0xFFFF)
    def w32(self, a, v): self.m.lib.m_w32(self.i, a, v & 0xFFFFFFFF)

    def screen(self):
        buf = (ctypes.c_uint8 * (240 * 160 * 3))()
        self.m.lib.m_screen(self.i, buf)
        return bytes(buf)

    def save_state(self):
        n = self.m.lib.m_state_size(self.i)
        buf = (ctypes.c_uint8 * n)()
        if self.m.lib.m_save_state(self.i, buf) != 0:
            raise RuntimeError('saveState failed')
        return bytes(buf)

    def load_state(self, data):
        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        if self.m.lib.m_load_state(self.i, buf) != 0:
            raise RuntimeError('loadState failed')
