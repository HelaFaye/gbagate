#!/usr/bin/env python3
"""test_ldn.py -- GB-Link-Switch-LDN's link task against the gbagate firmware.

    test_ldn.py LDN_LINK_HOST

LDN_LINK_HOST is ldn_link_host.c built around LDN's own pico_link.c (run.sh
does that). Its UART is wired to the simulator's bridge UART, byte for byte,
so this is LDN's code talking to GB-Link's firmware on gbagate the way an
ESP32 running LDN talks to a GB-Link over GP8/GP9. The checks are the ones
LDN's own pico_link_test.c makes of its scripted adapter:

  - the board puts the adapter in the wireless mode (SetMode 0x07 -> AwaitMode);
  - a working link is left alone, with a GBA clocking it too: LDN must not
    send SetMode again, since re-entering the mode resets the adapter under
    the game;
  - a USB host that only queries the adapter is answered on USB while the
    mode's telemetry stays with LDN; one that starts a mode takes the
    adapter over, and LDN notices the silence and takes it back.
"""
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'gblink_sim'))
from simdrv import Sim  # noqa: E402
import test_rfu as T  # noqa: E402


class Ldn:
    """ldn_link_host, its UART spliced onto a simulator's bridge."""

    def __init__(self, exe, sim, run_ms):
        self.p = subprocess.Popen([exe, str(run_ms)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.cv = threading.Condition()
        self.lines = []
        self.frames = []
        self.state = {}
        self.sim = sim
        sim.on_bridge = self._to_ldn
        threading.Thread(target=self._from_ldn, daemon=True).start()
        threading.Thread(target=self._report, daemon=True).start()

    def _to_ldn(self, data):
        try:
            self.p.stdin.write(data)
        except (BrokenPipeError, ValueError):
            pass

    def _from_ldn(self):
        while True:
            data = self.p.stdout.read1(4096) if hasattr(self.p.stdout, 'read1') else os.read(self.p.stdout.fileno(), 4096)
            if not data:
                return
            self.sim.bridge_raw(data)

    def _report(self):
        for raw in self.p.stderr:
            line = raw.decode().strip()
            with self.cv:
                self.lines.append(line)
                word, _, rest = line.partition(' ')
                if word in ('state', 'end'):
                    self.state = {k: int(v) for k, v in (kv.split('=') for kv in rest.split())}
                elif word == 'frame':
                    ch, payload = rest.split()
                    self.frames.append((int(ch), bytes.fromhex(payload)))
                self.cv.notify_all()

    def wait(self, pred, timeout, what):
        end = time.time() + timeout
        with self.cv:
            while not pred(self.state):
                left = end - time.time()
                if left <= 0:
                    raise AssertionError('%s; LDN state %s' % (what, self.state))
                self.cv.wait(left)
            return dict(self.state)

    def close(self):
        self.sim.on_bridge = None
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            self.p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.p.kill()


def check(cond, what):
    if not cond:
        raise AssertionError(what)


def main(argv):
    exe = argv[1]
    s = Sim()
    ldn = Ldn(exe, s, 70000)   # longer than every wait below added up
    ok = True
    t0 = time.time()
    try:
        st = ldn.wait(lambda st: st.get('await'), 5, 'LDN puts the adapter in the wireless mode')
        check(st['mode_sent'] == 1, 'one SetMode was enough: %s' % st)
        print('  ok    board enters the wireless mode (SetMode 0x07 -> AwaitMode)')

        # A GBA finds the adapter and keeps talking to it, as FireRed does
        # while it sits in the Union Room. Telemetry tells LDN it is there.
        g = T.Game(s)
        try:
            rid = g.check_id()
        except T.Skip as e:
            rid = None
            print('  (GBA part skipped: %s)' % e)
        if rid is not None:
            check(rid == 0x8001, 'the GBA found the adapter: %#x' % rid)
            end = time.time() + 6
            while time.time() < end:
                g.ok(T.SYSTEM_STATUS)
                time.sleep(0.05)
            st = ldn.wait(lambda st: st.get('gba'), 2, 'LDN sees the GBA clocking the adapter (0x0E dbgAnyRx)')
            print('  ok    LDN sees the GBA on the adapter')
        time.sleep(4)
        st = ldn.wait(lambda st: True, 1, '')
        check(st['mode_sent'] == 1 and st['await'], 'a working link is left alone: %s' % st)
        print('  ok    a working link is left alone (one SetMode in %.0f s)' % (time.time() - t0))

        # A USB host that only asks (the launcher polling GetFirmwareInfo) is
        # answered on USB; the mode's telemetry stays with LDN (PATCHES.md
        # P11), so LDN never sees silence and never re-enters the mode.
        for _ in range(16):
            mark = len(s.data)
            s.cmd(0x0F)
            s.wait_data(lambda d: d[0] == 0x0F, start=mark)
            time.sleep(0.25)
        st = ldn.wait(lambda st: True, 1, '')
        check(st['mode_sent'] == 1 and st['await'], 'a USB host querying for 4 s left the link alone: %s' % st)
        print('  ok    a USB host only querying leaves the link alone (still one SetMode)')

        # A USB host that starts a mode does take the adapter over; LDN, hearing
        # nothing, takes it back.
        s.cmd(0x00, 0x07, 0x00)
        st = ldn.wait(lambda st: st.get('mode_sent', 0) >= 2 and st.get('await'), 9,
                      'LDN takes the adapter back after a USB host set a mode')
        print('  ok    an adapter a USB host took over is taken back (%d SetModes)' % st['mode_sent'])
        time.sleep(3)
        st = ldn.wait(lambda st: True, 1, '')
        check(st['mode_sent'] == 2, 'and then left alone again: %s' % st)
        check(st['resync'] == 0 and st['dropped'] == 0, 'every byte framed, nothing dropped: %s' % st)
        print('  ok    clean framing: %d frames in, %d out, 0 resyncs, 0 dropped' % (st['rx'], st['tx']))
    except AssertionError as e:
        ok = False
        print('  FAIL  %s' % e)
        print('\n'.join('        ' + l for l in ldn.lines[-15:]))
    finally:
        ldn.close()
        s.close()
    print('ldn            %s' % ('LDN pico_link against gbagate: ok' if ok else 'FAILED'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
