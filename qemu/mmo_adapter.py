"""mmo_adapter.py -- a GBA's link words through an emulated adapter: the
touch carrier's hybrid image (GBAGATE_UART_BRIDGE) on qemu_esp32, with its
real board drivers, its link words on UART1 (the bridge, link_proto) and its
way to the MMO server on UART2 (the tunnel).

    Adapter(i, flash, out)  boots board i; .start_overworld() takes it to
    Link: Overworld by touch; .transfer(word) is one link transfer.

Used by test_mmo_adapter.py (model GBAs) and test/mgba/overworld_adapter.py
(Emerald).
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import accurate_boot as AB   # noqa: E402
import lp                    # noqa: E402

ROOT = os.path.dirname(HERE)
QEMU = os.environ.get('QEMU_XTENSA', os.path.join(ROOT, 'build/esp-qemu/build/qemu-system-xtensa'))
FLASH = os.environ.get('GBAGATE_HYBRID_FLASH',
                       os.path.join(ROOT, 'build/hw-gate-carrier-touch-mgba/flash-qemu.bin'))


class Adapter:
    BASE_PORT = 5600

    def __init__(self, i, out, flash=FLASH):
        self.i = i
        self.out = os.path.join(out, 'board%d' % i)
        os.makedirs(self.out, exist_ok=True)
        self.console = os.path.join(self.out, 'console.log')
        self.link_port = self.BASE_PORT + 10 * i
        self.net_port = self.link_port + 1
        sock = os.path.join(self.out, 'qmp.sock')
        for p in (self.console, sock):
            if os.path.exists(p):
                os.remove(p)
        fl = os.path.join(self.out, 'flash.bin')
        shutil.copy(flash, fl)                    # each board its own NVS
        args = [QEMU, '-machine', 'esp32s3', '-m', '8M',
                '-drive', 'file=%s,if=mtd,format=raw' % fl,
                '-serial', 'file:%s' % self.console,
                # nodelay: a transfer is a few bytes each way, and Nagle with
                # delayed ACKs held each one ~25-50 ms
                '-serial', 'tcp::%d,server,nowait,nodelay=on' % self.link_port,
                '-serial', 'tcp::%d,server,nowait,nodelay=on' % self.net_port,
                '-display', 'none', '-no-reboot', '-monitor', 'none',
                '-qmp', 'unix:%s,server=on,wait=off' % sock] + AB.board_args('carrier-touch', self.out)
        if os.environ.get('QEMU_BIOS'):
            args += ['-L', os.environ['QEMU_BIOS']]
        self.proc = subprocess.Popen(args, stdout=open(os.path.join(self.out, 'qemu.log'), 'w'),
                                     stderr=subprocess.STDOUT)
        self.qmp = AB.QMP(sock)
        self.seq = 0
        self.link = None
        self.dec = lp.Decoder()
        self.words = 0
        self.resent = 0

    def log(self):
        try:
            return open(self.console, errors='replace').read()
        except OSError:
            return ''

    def wait_log(self, text, seconds=60):
        end = time.time() + seconds
        while time.time() < end:
            if text in self.log():
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.1)
        return False

    def caps(self):
        m = re.search(r'shell started with caps 0x([0-9a-fA-F]+)', self.log())
        return int(m.group(1), 16) if m else None

    def tap(self, key, hold=0.15, gap=0.35):
        x, y = AB.key_centre(key)
        self.qmp.touch(x, y, True)
        time.sleep(hold)
        self.qmp.touch(x, y, False)
        time.sleep(gap)

    def start_overworld(self):
        """Link, then the chooser's Overworld, by touch. The menu opens on
        Link (row 0); the chooser lists Wireless, Online, Overworld here."""
        self.tap('A')
        self.tap('DOWN')
        self.tap('DOWN')
        self.tap('A')
        return self.wait_log("overworld: the agent's words to the server", 10)

    def connect_link(self, timeout=30):
        """The bridge: link_proto over UART1, the host side of the cable."""
        end = time.time() + timeout
        while True:
            try:
                self.link = socket.create_connection(('127.0.0.1', self.link_port), timeout=2)
                break
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(0.2)
        self.link.settimeout(0.5)
        self.link.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for _ in range(40):                        # HELLO until the firmware answers
            self.link.sendall(lp.Packet(type=lp.HELLO, seq=0, role=lp.ROLE_SLAVE).encode())
            for p in self._recv():
                if p.type in (lp.HELLO, lp.HELLO_ACK):
                    return True
        return False

    def _recv(self):
        try:
            data = self.link.recv(4096)
        except socket.timeout:
            return []
        return self.dec.feed(data) or []

    def transfer(self, word, timeout=3.0):
        """One link transfer: our word out, the board's back (its slot, 1).
        A word the board did not take (one arriving as it switches mode) is
        sent again; a transfer that never completes raises."""
        self.seq = (self.seq + 1) & 0xFFFF
        pkt = lp.Packet(type=lp.WORD, seq=self.seq, word=word).encode()
        end = time.time() + timeout
        first = True
        while time.time() < end:
            if not first:
                # The bridge drops a silent session and HELLOs until answered
                # (src/link_bridge.c): say hello before the word again.
                self.link.sendall(lp.Packet(type=lp.HELLO, seq=0, role=lp.ROLE_SLAVE).encode())
                self.resent += 1
            first = False
            self.link.sendall(pkt)
            retry = time.time() + 0.25
            while time.time() < retry:
                for p in self._recv():
                    if p.type == lp.HELLO:
                        self.link.sendall(lp.Packet(type=lp.HELLO_ACK, seq=p.seq,
                                                    role=lp.ROLE_SLAVE).encode())
                    if p.type == lp.RESULT and p.seq == self.seq:
                        self.words += 1
                        return p.words[1]
        raise TimeoutError('board %d: no RESULT for word %#06x' % (self.i, word))

    def shot(self, path):
        return self.qmp.shot(path)

    def stop(self):
        if self.link:
            self.link.close()
        self.proc.kill()
        self.proc.wait()
