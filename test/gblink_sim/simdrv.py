"""simdrv.py -- drive build/gblink_sim from Python tests.

Speaks the record format in sim_port.c. Collects everything the firmware sends
on background threads, so tests can wait for a status word or a data packet
the way a host would.
"""
import os
import struct
import subprocess
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SIM = os.environ.get('GBLINK_SIM') or os.path.join(ROOT, 'build', 'gblink_sim', 'gblink_sim')

EV_VCC, EV_LED, EV_REBOOT, EV_BOOTLOADER, EV_FLASH, EV_PHY, EV_WORD, EV_GB, EV_READY = range(1, 10)

ST = dict(AwaitMode=0xFF02, HandshakeReceived=0xFF03, LinkConnected=0xFF05,
          LinkReconnecting=0xFF06, LinkClosed=0xFF07, DeviceReady=0xFF08,
          EmuTradeSessionFinished=0xFF09, GBModeActive=0xFF0A,
          GBPrinterModeActive=0xFF0B, GBSessionFinished=0xFF0C)


class Sim:
    def __init__(self, flash=None, serial=False):
        args = [SIM] + ([flash] if flash else [])
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.cv = threading.Condition()
        self.status = []      # u16 words, in order
        self.data = []        # bytes objects from the data IN endpoint / channel 1
        self.events = []      # (code, bytes)
        self.cdc = bytearray()
        self.bridge = bytearray()   # raw bytes on the bridge UART
        self.bridge_frames = []     # (channel, payload) parsed from them
        self.serial = serial
        self.on_radio = None   # callable(bytes): datagrams the peer engine sends
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()
        self.wait_event(EV_READY)

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
                elif kind == ord('B'):
                    self.bridge += body
                    self._parse_bridge()
                elif kind == ord('P'):
                    if self.on_radio:
                        self.on_radio(body)
                elif kind == ord('E'):
                    self.events.append((ep, body))
                self.cv.notify_all()

    def _parse_cdc(self):
        while True:
            i = self.cdc.find(b'GB')
            if i < 0 or len(self.cdc) < i + 5:
                return
            ch = self.cdc[i + 2]
            n = self.cdc[i + 3] | (self.cdc[i + 4] << 8)
            if len(self.cdc) < i + 5 + n:
                return
            body = bytes(self.cdc[i + 5:i + 5 + n])
            del self.cdc[:i + 5 + n]
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

    def _send(self, kind, ep, body=b''):
        self.p.stdin.write(struct.pack('<BBH', ord(kind), ep, len(body)) + bytes(body))
        self.p.stdin.flush()

    def close(self):
        try:
            self._send('Q', 0)
            self.p.wait(timeout=2)
        except Exception:
            self.p.kill()

    # -- host side -------------------------------------------------------
    def cmd(self, *b):
        body = bytes(b[0]) if len(b) == 1 and not isinstance(b[0], int) else bytes(b)
        if self.serial:
            self._send('C', 0, b'GB\x00' + struct.pack('<H', len(body)) + body)
        else:
            self._send('U', 0x01, body)

    def write_data(self, body):
        body = bytes(body)
        if self.serial:
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
