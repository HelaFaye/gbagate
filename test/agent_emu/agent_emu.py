"""agent_emu.py -- the compiled memory agent (rom/agent.mb) on an emulated
ARM7, driven by the board's own client (src/mbagent.c) through ctypes.

test_mbagent.c tests the protocol core and the client together, as C on the
host. This runs what goes to a console: the actual bytes Send Program
transfers, from the multiboot entry point, with the SIO and timer registers
modelled as the agent uses them. So it catches what the host build cannot --
a register written in the wrong order, a link script that moved the core, a
step too slow for the gap the board leaves between words.

    python3 -I test/agent_emu/agent_emu.py build/libmbagent_shim.so rom/agent.mb

The timing model is deliberately pessimistic: every instruction costs 6
cycles (an ARM fetch from EWRAM, 16-bit and two wait states, is 6), and the
agent must restage within the 36 us mb_spi_xfer() waits after each word.
Needs the unicorn module (pip install unicorn); without it, it says so and
exits 77, which the Makefile reports as skipped unless REQUIRE_EMU is set.
"""
import ctypes
import struct
import sys

try:
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ, UC_HOOK_CODE
    from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_CPSR
except ImportError:
    print('agent-emu      skipped: no unicorn module (pip install unicorn)')
    sys.exit(77)

CLOCK_HZ = 16_777_216
CYCLES_PER_INSN = 6
GAP_US = 36                       # mb_spi_xfer()'s pause after every word
SIODATA32, SIOCNT, TM3CNT_L = 0x04000120, 0x04000128, 0x0400010C
SIO_START = 0x80
AG_IDLE = 0x454C4449
AG_ST_OK, AG_ST_PROTECTED = 0x59414B4F, 0x544F5250
MBA_OK, MBA_ERR_REFUSED = 0, 5

fails = 0


def check(what, cond):
    global fails
    if not cond:
        print('  FAIL', what)
        fails += 1


class GBA:
    def __init__(self, image):
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        for base, size in ((0x02000000, 0x40000), (0x03000000, 0x8000), (0x04000000, 0x1000),
                           (0x05000000, 0x1000), (0x06000000, 0x18000), (0x07000000, 0x1000)):
            uc.mem_map(base, size)
        uc.mem_write(0x02000000, image)
        uc.reg_write(UC_ARM_REG_PC, 0x020000C0)      # the BIOS jumps to the RAM entry
        self.insns = 0
        self.armed = False
        self.since_transfer = None                   # instructions since the last word
        self.worst_restage = 0
        self.missed = 0                              # words clocked too soon
        self.trace = bool(__import__('os').environ.get('AGENT_TRACE'))
        uc.hook_add(UC_HOOK_CODE, self._count)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._io_write, begin=SIOCNT, end=SIOCNT + 1)
        uc.hook_add(UC_HOOK_MEM_READ, self._timer_read, begin=TM3CNT_L, end=TM3CNT_L + 1)

    # -- the hardware the agent touches --------------------------------
    def _count(self, uc, addr, size, user):
        # Cycles by where the code runs: IWRAM is 32-bit with no wait states
        # (charged 2, for the data access an instruction may make); EWRAM is
        # 16-bit with two (6 for an ARM fetch).
        c = 2 if 0x03000000 <= addr < 0x03008000 else CYCLES_PER_INSN
        self.insns += c / CYCLES_PER_INSN
        if self.since_transfer is not None:
            self.since_transfer += c / CYCLES_PER_INSN

    def _io_write(self, uc, access, addr, size, value, user):
        if value & SIO_START:
            # Armed: the next word can be clocked. Stop so the board can.
            self.armed = True
            if self.since_transfer is not None:
                self.worst_restage = max(self.worst_restage, self.since_transfer)
                self.since_transfer = None
            # Not uc.emu_stop() here: unicorn resumes by re-running the
            # store that triggered the hook, so the agent would never get
            # past it. run_until_armed() runs in small slices instead.

    def _timer_read(self, uc, access, addr, size, value, user):
        ticks = int(self.insns * CYCLES_PER_INSN // 1024) & 0xFFFF   # prescaler 1024
        uc.mem_write(TM3CNT_L, struct.pack('<H', ticks))

    def run(self, insns):
        pc = self.uc.reg_read(UC_ARM_REG_PC)
        if self.uc.reg_read(UC_ARM_REG_CPSR) & 0x20:
            pc |= 1
        self.uc.emu_start(pc, 0xFFFFFFFF, count=insns)

    def run_for_us(self, us):
        # Modelled time, not an instruction count: IWRAM code fits three
        # times the instructions into a microsecond that EWRAM code does.
        target = self.insns + us * CLOCK_HZ / 1_000_000 / CYCLES_PER_INSN
        while self.insns < target:
            self.run(64)

    def run_until_armed(self, limit=2_000_000):
        while not self.armed and limit > 0:
            self.run(min(limit, 64))
            limit -= 64
        return self.armed

    # -- the board's side of one transfer -------------------------------
    def transfer(self, word):
        # The board clocks on its own schedule, armed or not: a word clocked
        # while the agent is still busy is lost, and the board reads noise.
        if not self.armed:
            self.missed += 1
            if self.trace:
                print('  missed: word %08x, pc %08x' % (word, self.uc.reg_read(UC_ARM_REG_PC)))
            return 0xFFFFFFFF
        uc = self.uc
        back = struct.unpack('<I', uc.mem_read(SIODATA32, 4))[0]
        uc.mem_write(SIODATA32, struct.pack('<I', word))
        cnt = struct.unpack('<H', uc.mem_read(SIOCNT, 2))[0]
        uc.mem_write(SIOCNT, struct.pack('<H', cnt & ~SIO_START))   # done: start clears
        self.armed = False
        self.since_transfer = 0
        self.run_for_us(GAP_US)                      # the gap mb_spi_xfer() leaves
        return back

    def wait_us(self, us):
        # The board chose to wait (a FILL, a CALL, a resync): what the agent
        # does meanwhile is not held to the per-word gap.
        self.since_transfer = None
        self.run_for_us(us)

    def read16(self, a):
        return struct.unpack('<H', self.uc.mem_read(a, 2))[0]

    def read32(self, a):
        return struct.unpack('<I', self.uc.mem_read(a, 4))[0]


def main(lib_path, image_path):
    image = open(image_path, 'rb').read()
    gba = GBA(image)
    lib = ctypes.CDLL(lib_path)
    XFER = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32)
    WAIT = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32)
    xfer = XFER(lambda ctx, w: gba.transfer(w))
    wait = WAIT(lambda ctx, us: gba.wait_us(us))
    lib.t_init(xfer, wait)
    u32 = ctypes.c_uint32

    check('the agent starts and arms the link', gba.run_until_armed())
    check('the backdrop is blue while it waits', gba.read16(0x05000000) == (0 | 4 << 5 | 20 << 10))

    ver, free = u32(), u32()
    check('PING', lib.t_ping(ctypes.byref(ver), ctypes.byref(free)) == MBA_OK)
    check('version 1', ver.value == 1)
    check('free EWRAM after the agent', 0x02000000 < free.value <= 0x02000000 + len(image) + 0x400)
    check('green once pinged', gba.read16(0x05000000) == (0 | 20 << 5 | 6 << 10))

    words = [(0xA5000000 | i * 7919) & 0xFFFFFFFF for i in range(600)]
    arr = (u32 * 600)(*words)
    check('write 600 words to EWRAM', lib.t_write(0x02010000, arr, 600) == MBA_OK)
    check('... are in its memory', all(gba.read32(0x02010000 + 4 * i) == words[i] for i in range(600)))
    back = (u32 * 600)()
    check('read them back', lib.t_read(0x02010000, back, 600) == MBA_OK)
    check('... the same', list(back) == words)

    check('poke the backdrop red', lib.t_poke16(0x05000000, 0x001F) == MBA_OK)
    check('... the screen changes', gba.read16(0x05000000) == 0x001F)
    check('fill VRAM', lib.t_fill(0x06000000, 2048, 0x12345678) == MBA_OK)
    check('... all of it', gba.read32(0x06000000) == 0x12345678 and gba.read32(0x06000000 + 4 * 2047) == 0x12345678)

    first = gba.read32(0x02000100)
    one = (u32 * 1)(0)
    check('a write over the agent is refused', lib.t_write(0x02000100, one, 1) == MBA_ERR_REFUSED)
    check('... PROT', lib.t_status() == AG_ST_PROTECTED)
    check('... and the agent is untouched', gba.read32(0x02000100) == first)

    # A routine in IWRAM: store 0xAB at 0x02030000 and return.
    code = struct.pack('<5I', 0xE3A00402, 0xE2800803, 0xE3A010AB, 0xE5801000, 0xE12FFF1E)
    code_words = (u32 * 5)(*struct.unpack('<5I', code))
    check('the routine written to free IWRAM', lib.t_write(0x03004000, code_words, 5) == MBA_OK)
    check('CALL it', lib.t_call(0x03004000, 200) == MBA_OK)
    check('the agent\'s own IWRAM code is protected', lib.t_write(0x03000000, code_words, 1) == MBA_ERR_REFUSED)
    check('... it ran', gba.read32(0x02030000) == 0xAB)

    # The board restarts mid-command: the agent times out and answers again.
    gba.transfer(0xA6030000 | 20)        # WRITE 20 words
    gba.transfer(0x02020000)
    gba.transfer(0x11111111)
    lib.t_init(xfer, wait)
    check('after a broken command, PING recovers', lib.t_ping(ctypes.byref(ver), ctypes.byref(free)) == MBA_OK)
    check('... by waiting out the timeout', lib.t_resyncs() >= 1)

    check('no word was clocked before the agent was ready (%d were)' % gba.missed, gba.missed == 0)
    restage_us = gba.worst_restage * CYCLES_PER_INSN * 1_000_000 / CLOCK_HZ
    check('every word restaged within the %d us gap (worst %.1f us)' % (GAP_US, restage_us),
          restage_us < GAP_US)
    print('agent-emu      %s (%d transfers; worst restage %.1f us of %d)' % (
        'all checks passed' if not fails else 'FAILURES', lib.t_words(), restage_us, GAP_US))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
