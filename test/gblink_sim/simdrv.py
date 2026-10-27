"""simdrv.py -- drive build/gblink_sim from Python tests.

Speaks the record format in sim_port.c. Collects everything the firmware sends
on background threads, so tests can wait for a status word or a data packet
the way a host would.
"""
import os
import struct
import subprocess
import sys
import threading
import time
import traceback

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SIM = os.environ.get('GBLINK_SIM') or os.path.join(ROOT, 'build', 'gblink_sim', 'gblink_sim')

EV_VCC, EV_LED, EV_REBOOT, EV_BOOTLOADER, EV_FLASH, EV_PHY, EV_WORD, EV_GB, EV_READY = range(1, 10)
EV_STATE = 12

ST = dict(AwaitMode=0xFF02, HandshakeReceived=0xFF03, LinkConnected=0xFF05,
          LinkReconnecting=0xFF06, LinkClosed=0xFF07, DeviceReady=0xFF08,
          EmuTradeSessionFinished=0xFF09, GBModeActive=0xFF0A,
          GBPrinterModeActive=0xFF0B, GBSessionFinished=0xFF0C)


# A simulator that ended badly: a sanitizer report aborts it (the ASan build
# has -fno-sanitize-recover=all), and one that does not leave on 'Q' is stuck.
# run_tests() fails the test during which one was seen.
BAD_EXITS = []


def note_exit(p, what):
    if p.returncode not in (0,):
        BAD_EXITS.append('%s exited %s' % (what, p.returncode))


def eventually(cond, timeout, what):
    """Poll cond() until it holds; AssertionError(what) if it never does.
    In place of a fixed sleep, which is too short on a slow machine and
    wasted on a fast one."""
    end = time.time() + timeout
    while True:
        v = cond()
        if v:
            return v
        if time.time() >= end:
            raise AssertionError(what() if callable(what) else what)
        time.sleep(0.02)


class Skip(Exception):
    """A test that cannot run here (no librfu, no browser)."""


def skip(why):
    """Skip -- unless GBLINK_NO_SKIP is set, as CI sets it: there a test that
    cannot run is a failure, not a pass."""
    if os.environ.get('GBLINK_NO_SKIP'):
        raise AssertionError('required, but cannot run: %s' % why)
    raise Skip(why)


def run_tests(name, tests, argv):
    """The suites' shared main: -k NAME runs the tests whose name has NAME
    (none matching is an error); the last line is the verdict."""
    pick = argv[argv.index('-k') + 1] if '-k' in argv else ''
    fails = skips = ran = 0
    for t in tests:
        if pick and pick not in t.__name__:
            continue
        ran += 1
        t0 = time.time()
        bad = len(BAD_EXITS)
        try:
            t()
            if len(BAD_EXITS) > bad:
                raise AssertionError('; '.join(BAD_EXITS[bad:]))
            print('  ok    %-40s %5.2fs' % (t.__name__, time.time() - t0))
        except Skip as e:
            skips += 1
            print('  skip  %-40s %s' % (t.__name__, e))
        except Exception:
            fails += 1
            print('  FAIL  %s' % t.__name__)
            traceback.print_exc(file=sys.stdout)
    if not ran:
        verdict = 'no test matches %r' % pick
        fails = 1
    elif fails:
        verdict = '%d of %d FAILED' % (fails, ran)
    elif skips:
        verdict = '%d passed, %d skipped' % (ran - skips, skips)
    else:
        verdict = 'all %d checks passed' % ran
    print('%-14s %s' % (name, verdict))
    return 1 if fails else 0


class Sim:
    def __init__(self, flash=None, serial=False, net=False):
        args = [SIM] + ([flash] if flash else [])
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.cv = threading.Condition()
        self._wlock = threading.Lock()
        self.status = []      # u16 words, in order
        self.data = []        # bytes objects from the data IN endpoint / channel 1
        self.events = []      # (code, bytes)
        self.cdc = bytearray()
        self.bridge = bytearray()   # raw bytes on the bridge UART
        self.bridge_frames = []     # (channel, payload) parsed from them
        self.serial = serial
        self.net = net         # host frames over the WiFi WebSocket ('N')
        self.netbuf = bytearray()
        self.net_bytes = 0     # bytes the board sent that way
        self.on_radio = None   # callable(bytes): datagrams the peer engine sends
        self.on_bridge = None  # callable(bytes): raw bridge UART bytes, as they come
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()
        try:
            self.wait_event(EV_READY)
        except BaseException:
            self.p.kill()
            self.p.wait()
            raise

    # -- plumbing --------------------------------------------------------
    def _reader(self):
        f = self.p.stdout
        while True:
            hdr = f.read(4)
            if len(hdr) < 4:
                return
            kind, ep, n = hdr[0], hdr[1], hdr[2] | (hdr[3] << 8)
            body = f.read(n) if n else b''
            with self.cv:
                if kind == ord('U') and ep == 0x81:
                    for i in range(0, len(body) - 1, 2):
                        self.status.append(body[i] | (body[i + 1] << 8))
                elif kind == ord('U') and ep == 0x82:
                    self.data.append(body)
                elif kind == ord('C'):
                    self.cdc += body
                    self._parse_cdc()
                elif kind == ord('N'):
                    self.net_bytes += len(body)
                    self.netbuf += body
                    self._parse_stream(self.netbuf)
                elif kind == ord('B'):
                    if self.on_bridge:
                        self.on_bridge(body)
                    self.bridge += body
                    self._parse_bridge()
                elif kind == ord('P'):
                    if self.on_radio:
                        self.on_radio(body)
                elif kind == ord('E'):
                    self.events.append((ep, body))
                self.cv.notify_all()

    def _parse_cdc(self):
        self._parse_stream(self.cdc)

    def _parse_stream(self, buf):
        while True:
            i = buf.find(b'GB')
            if i < 0 or len(buf) < i + 5:
                return
            ch = buf[i + 2]
            n = buf[i + 3] | (buf[i + 4] << 8)
            if len(buf) < i + 5 + n:
                return
            body = bytes(buf[i + 5:i + 5 + n])
            del buf[:i + 5 + n]
            if ch == 2:
                self.status.append(body[0] | (body[1] << 8))
            elif ch == 1:
                self.data.append(body)

    def _parse_bridge(self):
        while True:
            i = self.bridge.find(b'GB')
            if i < 0 or len(self.bridge) < i + 5:
                return
            n = self.bridge[i + 3] | (self.bridge[i + 4] << 8)
            if len(self.bridge) < i + 5 + n:
                return
            self.bridge_frames.append((self.bridge[i + 2], bytes(self.bridge[i + 5:i + 5 + n])))
            del self.bridge[:i + 5 + n]

    def bridge_send(self, channel, payload):
        """A 'GB' frame on the bridge UART, as a companion MCU sends one."""
        payload = bytes(payload)
        self._send('B', 0, b'GB' + bytes([channel]) + struct.pack('<H', len(payload)) + payload)

    def bridge_raw(self, data):
        """Bytes onto the bridge UART as they are, framed or not."""
        self._send('B', 0, bytes(data))

    def _send(self, kind, ep, body=b''):
        # one record at a time: a relay or a bridge pump writes from its own thread
        with self._wlock:
            self.p.stdin.write(struct.pack('<BBH', ord(kind), ep, len(body)) + bytes(body))
            self.p.stdin.flush()

    def close(self):
        try:
            self._send('Q', 0)
            self.p.wait(timeout=2)
        except Exception:
            self.p.kill()
            self.p.wait()
        note_exit(self.p, 'gblink_sim')
        try:
            self.p.stdin.close()
        except (BrokenPipeError, ValueError, OSError):
            pass

    # -- host side -------------------------------------------------------
    def cmd(self, *b):
        body = bytes(b[0]) if len(b) == 1 and not isinstance(b[0], int) else bytes(b)
        if self.net:
            self._send('N', 0, b'GB\x00' + struct.pack('<H', len(body)) + body)
        elif self.serial:
            self._send('C', 0, b'GB\x00' + struct.pack('<H', len(body)) + body)
        else:
            self._send('U', 0x01, body)

    def write_data(self, body):
        body = bytes(body)
        if self.net:
            self._send('N', 0, b'GB\x01' + struct.pack('<H', len(body)) + body)
        elif self.serial:
            self._send('C', 0, b'GB\x01' + struct.pack('<H', len(body)) + body)
        else:
            self._send('U', 0x02, body)

    def raw_cdc(self, body):
        self._send('C', 0, bytes(body))

    def radio(self, dgram):
        self._send('P', 0, bytes(dgram))

    def start_peer(self, nonce, mode=1, variant=0):
        self._send('S', 0, struct.pack('<IBB', nonce, mode, variant))

    def gba(self, *b):
        self._send('G', 0, bytes(b))

    def state(self, timeout=3.0):
        """gblink_state(), what the device's own Link: USB page shows."""
        with self.cv:
            mark = len(self.events)
        self._send('I', 0)
        b = self.wait_event(EV_STATE, timeout=timeout, start=mark)
        conf, mode, owner, status, statuses = struct.unpack('<BbBHI', b[:9])
        frames = struct.unpack('<5I', b[9:29])
        return dict(configured=bool(conf), mode=mode, owner=owner, status=status,
                    statuses=statuses, frames=frames)

    # -- waiting ---------------------------------------------------------
    def _wait(self, pred, timeout):
        end = time.time() + timeout
        with self.cv:
            while True:
                r = pred()
                if r is not None:
                    return r
                left = end - time.time()
                if left <= 0:
                    raise TimeoutError
                self.cv.wait(left)

    def wait_status(self, word, timeout=3.0, start=0):
        def p():
            for i in range(start, len(self.status)):
                if self.status[i] == word:
                    return i
            return None
        return self._wait(p, timeout)

    def wait_data(self, pred=lambda d: True, timeout=3.0, start=0):
        def p():
            for i in range(start, len(self.data)):
                if pred(self.data[i]):
                    return self.data[i]
            return None
        return self._wait(p, timeout)

    def wait_event(self, code, pred=lambda b: True, timeout=3.0, start=0):
        def p():
            for i in range(start, len(self.events)):
                c, b = self.events[i]
                if c == code and pred(b):
                    return b
            return None
        return self._wait(p, timeout)

    def words(self):
        with self.cv:
            return [struct.unpack('<HH', b) for c, b in self.events if c == EV_WORD]
