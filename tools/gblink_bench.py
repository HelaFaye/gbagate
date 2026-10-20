#!/usr/bin/env python3
"""gblink_bench.py -- bring a gbagate board up as a GB-Link adapter, one step
at a time, from a terminal.

Talks to the board's CDC-ACM port with GB-Link's 'GB' framing (the same
commands the web apps send over WebUSB), so it needs nothing but Python: no
browser, no libusb, no pyserial. doc/BRINGUP-GBLINK.md says what to plug in
for each step and what a good answer looks like.

    tools/gblink_bench.py PORT info          who it is, cable, counters
    tools/gblink_bench.py PORT watch [S]     link counters every half second
    tools/gblink_bench.py PORT led R G B     the status LED (0x42)
    tools/gblink_bench.py PORT gen3          Gen 3 link: handshake with a GBA
    tools/gblink_bench.py PORT rfu [S]       Wireless Adapter: is the GBA finding it?
    tools/gblink_bench.py PORT gate GAME CHIP  gbagate's chip gate: one chip

PORT is the serial device (/dev/ttyACM0, /dev/cu.usbmodem..., or COM5 on
Windows via a path the OS accepts), or "sim" for the host simulator
(build/gblink_sim), which is how this script is tested.
"""
import os
import struct
import subprocess
import sys
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

ST = {0xFF02: 'AwaitMode', 0xFF03: 'HandshakeReceived', 0xFF05: 'LinkConnected',
      0xFF06: 'LinkReconnecting', 0xFF07: 'LinkClosed', 0xFF08: 'DeviceReady',
      0xFF09: 'EmuTradeSessionFinished', 0xFF0A: 'GBModeActive',
      0xFF0B: 'GBPrinterModeActive', 0xFF0C: 'GBSessionFinished'}

PHY_FIELDS = ('mp_words', 'mp_dropped', 'mp_late', 'rfu_words', 'rfu_aborted',
              'link_words', 'uptime_ms')
RFU_ISR_FIELDS = ('worst_pass_us', 'late_passes', 'tightest_gap_us', 'passes', 'reset_pin')
RFU_COMSTATE = ('ID wait', 'ID exchange', 'wait command', 'wait data', 'respond',
                'respond data', 'error', 'error 2', 'wait event', 'adapter as master',
                'replay wait', 'replay data')


# ---- transports ---------------------------------------------------------------

class Framer:
    """'GB' | channel | len LE | payload, both ways."""

    def __init__(self):
        self.buf = bytearray()
        self.cv = threading.Condition()
        self.status = []   # u16
        self.data = []     # bytes

    def feed(self, b):
        with self.cv:
            self.buf += b
            while True:
                i = self.buf.find(b'GB')
                if i < 0 or len(self.buf) < i + 5:
                    break
                n = self.buf[i + 3] | (self.buf[i + 4] << 8)
                if len(self.buf) < i + 5 + n:
                    break
                ch, body = self.buf[i + 2], bytes(self.buf[i + 5:i + 5 + n])
                del self.buf[:i + 5 + n]
                if ch == 2 and len(body) >= 2:
                    self.status.append(body[0] | (body[1] << 8))
                elif ch == 1:
                    self.data.append(body)
            self.cv.notify_all()

    @staticmethod
    def frame(ch, payload):
        payload = bytes(payload)
        return b'GB' + bytes([ch]) + struct.pack('<H', len(payload)) + payload


class SerialPort(Framer):
    def __init__(self, path):
        super().__init__()
        import termios
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0                     # iflag: raw
        attrs[1] = 0                     # oflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0                     # lflag: no echo, no canonical mode
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 1
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while True:
            try:
                b = os.read(self.fd, 512)
            except OSError:
                return
            if b:
                self.feed(b)

    def send(self, ch, payload):
        os.write(self.fd, self.frame(ch, payload))

    def close(self):
        os.close(self.fd)


class SimPort(Framer):
    """build/gblink_sim, driven over its CDC records ('C'); .gba() reaches its
    GBA model, for tests."""

    def __init__(self, sim=None):
        super().__init__()
        sim = sim or os.environ.get('GBLINK_SIM') or os.path.join(ROOT, 'build', 'gblink_sim', 'gblink_sim')
        self.p = subprocess.Popen([sim], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.events = []
        self.ready = threading.Event()
        threading.Thread(target=self._reader, daemon=True).start()
        self.ready.wait(5)

    def _reader(self):
        f = self.p.stdout
        while True:
            hdr = f.read(4)
            if len(hdr) < 4:
                return
            n = hdr[2] | (hdr[3] << 8)
            body = f.read(n) if n else b''
            if hdr[0] == ord('C'):
                self.feed(body)
            elif hdr[0] == ord('E'):
                if hdr[1] == 9:
                    self.ready.set()
                with self.cv:
                    self.events.append((hdr[1], body))
                    self.cv.notify_all()

    def _record(self, kind, body):
        self.p.stdin.write(struct.pack('<BBH', ord(kind), 0, len(body)) + body)
        self.p.stdin.flush()

    def send(self, ch, payload):
        self._record('C', self.frame(ch, payload))

    def gba(self, *b):
        self._record('G', bytes(b))

    def close(self):
        """The simulator's exit status: 0 unless it crashed (a sanitizer
        report, under ASan) or would not quit."""
        try:
            self._record('Q', b'')
            self.p.wait(timeout=2)
        except Exception:
            self.p.kill()
            self.p.wait()
        try:
            self.p.stdin.close()
        except (BrokenPipeError, ValueError, OSError):
            pass
        return self.p.returncode


def open_port(name):
    return SimPort() if name == 'sim' else SerialPort(name)


# ---- the board ----------------------------------------------------------------------

class Board:
    def __init__(self, port):
        self.port = port

    def cmd(self, *b):
        self.port.send(0, bytes(b))

    def reply(self, cmd_id, timeout=1.0):
        """The next data packet starting with cmd_id, sent after this call."""
        with self.port.cv:
            start = len(self.port.data)
        end = time.time() + timeout
        with self.port.cv:
            while True:
                for d in self.port.data[start:]:
                    if d[:1] == bytes([cmd_id]):
                        return d
                left = end - time.time()
                if left <= 0:
                    return None
                self.port.cv.wait(left)

    def ask(self, *b, timeout=1.0):
        with self.port.cv:
            start = len(self.port.data)
        self.cmd(*b)
        end = time.time() + timeout
        with self.port.cv:
            while True:
                for d in self.port.data[start:]:
                    if d[:1] == bytes([b[0]]):
                        return d
                left = end - time.time()
                if left <= 0:
                    return None
                self.port.cv.wait(left)

    def status_since(self, mark):
        with self.port.cv:
            return self.port.status[mark:]

    def mark(self):
        with self.port.cv:
            return len(self.port.status), len(self.port.data)

    def wait_status(self, word, timeout, since):
        end = time.time() + timeout
        with self.port.cv:
            while True:
                if word in self.port.status[since:]:
                    return True
                left = end - time.time()
                if left <= 0:
                    return False
                self.port.cv.wait(left)

    def data_since(self, mark):
        with self.port.cv:
            return self.port.data[mark:]

    # the queries
    def firmware(self):
        d = self.ask(0x0F)
        if not d or len(d) < 4:
            return None
        info = {'version': '%d.%d.%d' % (d[1], d[2], d[3])}
        if len(d) >= 5:
            info['landing_page'] = bool(d[4])
        if len(d) >= 6:
            info['cable_selection'] = {0: 'auto', 1: 'GBA', 2: 'GBC'}.get(d[5], d[5])
        return info

    def gbx(self):
        d = self.ask(0x4F, timeout=0.5)
        if not d or len(d) < 16 or d[1:4] != b'GBX':
            return None
        caps = struct.unpack('<I', d[8:12])[0]
        names = ['chip gate', 'SD_ALT', 'RGB LED', 'VCC switch', 'display', 'storage',
                 'ESP-NOW', 'multiboot', 'bridge UART']
        return {'gbagate': '%d.%d.%d' % (d[5], d[6], d[7]),
                'board': {0: 'host simulator', 1: 'devkit', 2: 'Supermini'}.get(d[12], d[12]),
                'image': {0: 'link', 1: 'gate', 2: 'full', 3: 'mini'}.get(d[13], d[13]),
                'caps': [n for i, n in enumerate(names) if caps & (1 << i)]}

    def phy(self):
        d = self.ask(0x4E, timeout=0.5)
        # [0x4E, layout 1, u32 * 7]; another layout is not this one's to read.
        if not d or len(d) < 2 + 4 * len(PHY_FIELDS) or d[1] != 1:
            return None
        return dict(zip(PHY_FIELDS, struct.unpack('<%dI' % len(PHY_FIELDS), d[2:2 + 4 * len(PHY_FIELDS)])))

    def rfu_isr(self):
        d = self.ask(0x4C, timeout=0.5)
        if not d or len(d) < 21:
            return None
        return dict(zip(RFU_ISR_FIELDS, struct.unpack('<5I', d[1:21])))

    def cable(self):
        d = self.ask(0x4A, timeout=0.5)
        return None if not d or len(d) < 2 else ('GBA' if d[1] == 0 else 'GBC')

    def cancel(self):
        self.cmd(0x01)


# ---- the steps ------------------------------------------------------------------------

def say(*a):
    print(*a, flush=True)


def step_info(b, args):
    fw = b.firmware()
    if not fw:
        say('FAIL  no answer to GetFirmwareInfo (0x0F): wrong port, or the board is in drive mode (settings: usb)')
        return 1
    say('ok    GB-Link protocol %s, landing page %s, cable selection %s'
        % (fw['version'], fw.get('landing_page'), fw.get('cable_selection')))
    gx = b.gbx()
    say('ok    gbagate %(gbagate)s, %(board)s, %(image)s image, %(caps)s' % gx if gx else
        'note  no GBX block (0x4F): this is a GB-Link, not a gbagate')
    say('ok    cable in effect: %s' % b.cable())
    p = b.phy()
    if p:
        say('ok    link counters: ' + ', '.join('%s %d' % kv for kv in p.items()))
    return 0


def step_watch(b, args):
    secs = float(args[0]) if args else 10.0
    last = None
    end = time.time() + secs
    while time.time() < end:
        p = b.phy()
        if p and last:
            say('  ' + '  '.join('%s +%d' % (k, p[k] - last[k]) for k in PHY_FIELDS[:5]))
        last = p or last
        time.sleep(0.5)
    return 0


def step_led(b, args):
    r, g, bl = (int(x) for x in (args + ['32', '0', '32'])[:3])
    b.cmd(0x42, r, g, bl, 1)            # SetLEDColor: r, g, b, on
    say('ok    LED set to %d %d %d (live, not saved)' % (r, g, bl))
    return 0


def step_gen3(b, args):
    """The adapter as link master against a Gen 3 game in a link room: the
    GBA's handshake words coming back is the Multi-Play wire working."""
    b.cancel()
    time.sleep(0.1)
    m, _ = b.mark()
    before = b.phy() or {}
    b.cmd(0x00, 0x01)
    if not b.wait_status(0xFF02, 3, m):
        say('FAIL  no AwaitMode after SetMode(0x01)')
        return 1
    b.cmd(0x10)
    say('....  waiting for the GBA (be in a Cable Club room, as the other player)')
    ok = b.wait_status(0xFF03, 20, m)
    after = b.phy() or {}
    words = after.get('mp_words', 0) - before.get('mp_words', 0)
    if not ok:
        say('FAIL  no HandshakeReceived; %d words went over the wire' % words)
        say('      words 0: no clock reaches the GBA (SC, cable, VCC). words climbing:'
            ' the GBA answers but not with 0xB9A0 (SD/SO swapped, wrong cable path).')
        b.cancel()
        return 1
    say('ok    HandshakeReceived: the GBA answered the handshake (%d words, late %d, dropped %d)'
        % (words, after.get('mp_late', 0) - before.get('mp_late', 0),
           after.get('mp_dropped', 0) - before.get('mp_dropped', 0)))
    b.cancel()
    return 0


def step_rfu(b, args):
    """Mode 0x07 with a Wireless Adapter game: upstream's 500 ms telemetry
    says when the game has found the adapter."""
    secs = float(args[0]) if args else 30.0
    fw = b.firmware()
    if fw and tuple(int(x) for x in fw['version'].split('.')) < (2, 2, 7):
        say('FAIL  protocol %s has no Wireless Adapter mode' % fw['version'])
        return 1
    b.cancel()
    time.sleep(0.1)
    m, dm = b.mark()
    b.cmd(0x00, 0x07, 0x00)
    if not b.wait_status(0xFF02, 3, m):
        say('FAIL  no AwaitMode after SetMode(0x07)')
        return 1
    say('....  mode 0x07 up; start the game\'s wireless menu (FRLG: Union Room / wireless club)')
    found = False
    last_comstate = None
    resets0 = resets = None
    end = time.time() + secs
    while time.time() < end and not found:
        time.sleep(0.5)
        new = b.data_since(dm)
        dm += len(new)
        for d in new:
            if len(d) == 16 and d[0] == 0x0E:
                any_rx, comstate, xfers = d[1], d[3], struct.unpack('<I', d[12:16])[0]
                if comstate != last_comstate:
                    say('      comstate %s, any data %s, transfers %d'
                        % (RFU_COMSTATE[comstate] if comstate < len(RFU_COMSTATE) else comstate,
                           bool(any_rx), xfers))
                    last_comstate = comstate
                if xfers and not any_rx:
                    say('note  the GBA clocks but every word is 0: a GBA link cable grounds the'
                        ' receive line; use a GBC cable')
                if comstate >= 2:
                    found = True
            elif len(d) == 25 and d[0] == 0x1D:
                resets = d[14]
                resets0 = resets if resets0 is None else resets0
    isr = b.rfu_isr()
    phy = b.phy()
    if isr:
        say('      transfer path: worst pass %(worst_pass_us)d us, %(late_passes)d late, '
            'tightest gap %(tightest_gap_us)d us, %(passes)d passes' % isr)
    if phy:
        say('      words %(rfu_words)d, aborted mid-word %(rfu_aborted)d' % phy)
    if resets is not None:
        say('      soft resets from the game: %d' % ((resets - resets0) & 0xFF))
    if found:
        say('ok    the game found the adapter (ID exchange done)')
    else:
        say('FAIL  the game did not find the adapter in %ds' % secs)
    b.cancel()
    return 0 if found else 1


def step_gate(b, args):
    game = int(args[0]) if args else 4
    chip = int(args[1], 0) if len(args) > 1 else 0x012A
    b.cancel()
    time.sleep(0.1)
    m, _ = b.mark()
    b.cmd(0x00, 0x80, game)
    say('....  gate armed (game %d); open the chip gate screen in the game' % game)
    if not b.wait_status(0xFF05, 30, m):
        say('FAIL  the game never opened a gate session')
        b.cancel()
        return 1
    _, dm = b.mark()
    d = b.ask(0x50, chip & 0xFF, chip >> 8)
    if not (d and len(d) > 1 and d[1]):
        say('FAIL  session open, but chip %#06x was refused' % chip)
        b.cancel()
        return 1
    say('ok    session open; chip %#06x accepted' % chip)
    end = time.time() + 5
    sent = False
    while time.time() < end and not sent:
        sent = any(x[:1] == b'\x54' for x in b.data_since(dm))
        time.sleep(0.1)
    say('ok    chip sent' if sent else 'FAIL  no CHIP_SENT within 5 s')
    b.cancel()
    return 0 if sent else 1


STEPS = {'info': step_info, 'watch': step_watch, 'led': step_led, 'gen3': step_gen3,
         'rfu': step_rfu, 'gate': step_gate}


def main(argv):
    if len(argv) < 3 or argv[2] not in STEPS:
        print(__doc__)
        return 2
    port = open_port(argv[1])
    try:
        return STEPS[argv[2]](Board(port), argv[3:])
    finally:
        port.close()


if __name__ == '__main__':
    sys.exit(main(sys.argv))
