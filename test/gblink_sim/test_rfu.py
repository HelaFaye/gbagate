"""test_rfu.py -- the Wireless Adapter mode (0x07), with the game's own code.

The GBA end of the link port is pret's decompiled librfu -- the STWI command
layer, its SIO interrupt handler and the Sio32ID exchange that FireRed,
LeafGreen and Emerald link in -- running on a model of the GBA's serial,
timer and interrupt hardware (sim_rfu.c, rfu_game.c). The adapter end is
GB-Link's RFU core (sections/rfuProtocol*), on gbagate's link layer. Neither
side knows the other is simulated.

librfu is fetched from pret/pokefirered by fetch_librfu.sh; without it these
tests report themselves skipped.

    python3 -I test/gblink_sim/test_rfu.py [-k name]
"""
import os
import struct
import sys
import threading
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simdrv import Sim, ST  # noqa: E402

EV_RFU = 11
MODE_RFU = 0x07
TESTS = []

# librfu REQ ids (librfu.h) -- the adapter's command numbers.
RESET, LINK_STATUS, VERSION, SYSTEM_STATUS, SLOT_STATUS, CONFIG_STATUS = 0x10, 0x11, 0x12, 0x13, 0x14, 0x15
GAME_CONFIG, SYSTEM_CONFIG = 0x16, 0x17
SC_START, SC_POLL, SC_END = 0x19, 0x1A, 0x1B
SP_START, SP_POLL, SP_END = 0x1C, 0x1D, 0x1E
CP_START, CP_POLL, CP_END = 0x1F, 0x20, 0x21
DATA_TX, DATA_TX_AND_CHANGE, DATA_RX, MS_CHANGE = 0x24, 0x25, 0x26, 0x27
CONN_INPROGRESS = 0x01000000


def test(fn):
    TESTS.append(fn)
    return fn


def check(cond, what):
    if not cond:
        raise AssertionError(what)


class Skip(Exception):
    pass


class Game:
    """librfu on the GBA behind one simulated adapter."""

    def __init__(self, sim):
        self.s = sim

    def _event(self, pred, timeout, start):
        ev = self.s.wait_event(EV_RFU, pred=lambda b: b[0] == 0 or pred(b), timeout=timeout, start=start)
        if ev[0] == 0:
            raise Skip('librfu not built in (test/gblink_sim/fetch_librfu.sh)')
        return ev

    def check_id(self, tries=8):
        mark = len(self.s.events)
        self.s.gba(0x20, tries)
        ev = self._event(lambda b: b[0] == 1, 15, mark)
        return struct.unpack('<i', ev[1:5])[0]

    def soft_reset(self):
        self.s.gba(0x22)

    def req(self, rid, args=b'', timeout=5):
        """Issue a REQ, wait for it to end; returns (error, [ack words])."""
        mark = len(self.s.events)
        self.s.gba(0x21, rid, *bytes(args))
        ev = self._event(lambda b: b[0] == 2 and b[1] == rid, timeout, mark)
        err = ev[2] | (ev[3] << 8)
        words = list(struct.unpack('<%dI' % ((len(ev) - 4) // 4), ev[4:]))
        return err, words

    def ok(self, rid, args=b''):
        err, words = self.req(rid, args)
        check(err == 0, 'REQ 0x%02X failed with error %d' % (rid, err))
        check(words and words[0] >> 16 == 0x9966 and words[0] & 0xFF == 0x80 | rid,
              'REQ 0x%02X: ACK header %s' % (rid, hex(words[0]) if words else None))
        return words[1:]

    def slave_event(self, start, timeout=5):
        """librfu's slave callback: the adapter, as clock master, delivered a
        command. Returns (cmd id, [words])."""
        ev = self._event(lambda b: b[0] == 3, timeout, start)
        cmd = ev[1] | (ev[2] << 8)
        return cmd & 0xFF, list(struct.unpack('<%dI' % ((len(ev) - 3) // 4), ev[3:]))


def enter_wireless(s):
    mark = len(s.status)
    s.cmd(0x00, MODE_RFU, 0)
    s.wait_status(ST['AwaitMode'], start=mark)
    return Game(s)


def telemetry(s, kind, length, start=0):
    return s.wait_data(lambda d: len(d) == length and d[0] == kind, timeout=3, start=start)


class Relay:
    """What Celio, the netplay bridge or the Switch bridge does for mode 0x07:
    carry each adapter's 64-byte RFU1 stream chunks to the other."""

    def __init__(self, a, b):
        self.pairs = [(a, b), (b, a)]
        self.stop = False
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        seen = [0, 0]
        while not self.stop:
            for i, (src, dst) in enumerate(self.pairs):
                with src.cv:
                    new = src.data[seen[i]:]
                    seen[i] = len(src.data)
                for d in new:
                    if len(d) == 64:
                        dst.write_data(d)
            time.sleep(0.002)

    def close(self):
        self.stop = True
        self.t.join(1)


# ---- discovery -------------------------------------------------------------------

@test
def rfu_probe_reports_wireless():
    """The netplay bridge's GBLinkRfuDevice requires firmware 2.2.7 and a
    GetRfuIsrStats (0x4C) reply of 21 bytes before it enters mode 0x07."""
    s = Sim()
    try:
        s.cmd(0x0F)
        info = s.wait_data(lambda d: d[0] == 0x0F)
        check(tuple(info[1:4]) == (2, 2, 7), 'firmware 2.2.7: %s' % list(info[1:4]))
        s.cmd(0x4C)
        stats = s.wait_data(lambda d: d[0] == 0x4C)
        check(len(stats) == 21, 'GetRfuIsrStats is [0x4C, 5 x u32]: %d bytes' % len(stats))
    finally:
        s.close()


@test
def rfu_game_finds_adapter():
    """AgbRFU_checkID: the game's SIO32 ID exchange ("NINTENDO" against the
    adapter's complement dance) must converge on RFU_ID, 0x8001."""
    s = Sim()
    try:
        g = enter_wireless(s)
        rid = g.check_id()
        check(rid == 0x8001, 'checkID returned %#x' % rid)
        det = telemetry(s, 0x0E, 16, start=len(s.data))
        check(det[3] >= 2, 'detection telemetry: comstate %d past the ID exchange' % det[3])
    finally:
        s.close()


@test
def rfu_status_requests():
    """The REQs every librfu game sends after checkID, answered as a real
    AGB-015 answers them (gpsp's values; ConfigStatus per afska's notes)."""
    s = Sim()
    try:
        g = enter_wireless(s)
        check(g.check_id() == 0x8001, 'adapter found')
        g.ok(RESET)
        check(g.ok(VERSION) == [0x00830117], 'version word')
        check(g.ok(SYSTEM_STATUS) == [0], 'idle: no session')
        cfg = g.ok(CONFIG_STATUS)
        check(len(cfg) == 7 and cfg[6] == 0x101, 'ConfigStatus as a client: %s' % [hex(w) for w in cfg])
        check(g.ok(SLOT_STATUS) == [], 'no slots')
    finally:
        s.close()


@test
def rfu_soft_reset_restarts_the_adapter():
    """AgbRFU_SoftReset raises SD for ~1 ms through RCNT's general-purpose
    mode; the adapter must take it as a reset (upstream counts them in the
    event telemetry) and answer a fresh checkID."""
    s = Sim()
    try:
        g = enter_wireless(s)
        check(g.check_id() == 0x8001, 'first checkID')
        g.ok(RESET)
        before = telemetry(s, 0x1D, 25, start=len(s.data))[14]
        check(g.check_id() == 0x8001, 'checkID after a soft reset')
        after = telemetry(s, 0x1D, 25, start=len(s.data))[14]
        check((after - before) & 0xFF >= 1, 'soft resets counted: %d -> %d' % (before, after))
        check(g.ok(VERSION) == [0x00830117], 'commands after the reset')
    finally:
        s.close()


@test
def rfu_wait_returns_as_clock_master():
    """MS_Change (0x27) hands the clock to the adapter: the GBA arms as SIO
    slave, and the adapter, as master, must clock its event in through
    librfu's slave handshake and give the clock back so the next REQ works.
    With no session the event is gpsp's "disconnected" (0x29, status 0xF)."""
    s = Sim()
    try:
        g = enter_wireless(s)
        check(g.check_id() == 0x8001, 'adapter found')
        g.ok(RESET)
        mark = len(s.events)
        g.ok(MS_CHANGE)
        cmd, words = g.slave_event(mark, timeout=3)
        check(cmd == 0x29 and words[1] == 0xF, 'idle wait: disconnected (0x29, 0xF): %#x %s' % (cmd, [hex(w) for w in words]))
        check(g.ok(VERSION) == [0x00830117], 'GBA is clock master again')
    finally:
        s.close()


@test
def rfu_host_wait_reports_no_response():
    """A host with nobody answering: the wait ends when the retransmission
    window the game configured (SystemConfig's maxMFrame) runs out, as a data
    event marked "no response" (gpsp: 0x0F0F)."""
    s = Sim()
    try:
        g = enter_wireless(s)
        check(g.check_id() == 0x8001, 'adapter found')
        host_room(g)
        mark = len(s.events)
        g.ok(MS_CHANGE)
        cmd, words = g.slave_event(mark, timeout=3)
        check(cmd == 0x28 and words[1] == 0x0F0F, 'host wait: rtx data event: %#x %s' % (cmd, [hex(w) for w in words]))
        check(g.ok(SYSTEM_STATUS)[0] >> 24 == 1, 'still hosting')
    finally:
        s.close()


# ---- two adapters ---------------------------------------------------------------------

def host_room(g, name=b'GBAGATE ROOM\0\0', user=b'HOST\0\0\0\0'):
    g.ok(RESET)
    g.ok(SYSTEM_CONFIG, struct.pack('<HBB', 0x0000, 4, 32))
    g.ok(GAME_CONFIG, struct.pack('<H', 0x0202) + name + user)
    g.ok(SC_START)


def find_room(g, timeout=4):
    g.ok(RESET)
    g.ok(SP_START)
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(0.25)
        found = g.ok(SP_POLL)
        if found:
            return found
    raise AssertionError('no room found')


@test
def rfu_client_finds_and_joins_host():
    """The Union Room's shape, at the REQ level, between two adapters joined
    only by their RFU1 streams: the host's broadcast reaches the client's
    search, the client connects, and the host sees it in its slot list."""
    a, b = Sim(), Sim()
    relay = None
    try:
        ga, gb = enter_wireless(a), enter_wireless(b)
        check(ga.check_id() == 0x8001 and gb.check_id() == 0x8001, 'both adapters found')
        relay = Relay(a, b)
        host_room(ga)
        found = find_room(gb)
        check(len(found) >= 7, 'a room is 7 words: %s' % [hex(w) for w in found])
        host_id = found[0] & 0xFFFF
        game = struct.pack('<6I', *found[1:7])
        check(game[2:14] == b'GBAGATE ROOM' and game[16:20] == b'HOST',
              'the room carries the host game config: %r' % game)
        check(host_id == ga.ok(SYSTEM_STATUS)[0] & 0xFFFF, 'search shows the host adapter id')

        gb.ok(SP_END)
        gb.ok(CP_START, struct.pack('<H', host_id))
        end = time.time() + 4
        while True:
            st = gb.ok(CP_POLL)[0]
            if st != CONN_INPROGRESS or time.time() > end:
                break
            time.sleep(0.1)
        check(st & 0xFF000000 == 0, 'connected: %#x' % st)
        child_id = st & 0xFFFF
        gb.ok(CP_END)
        slots = ga.ok(SC_POLL)
        check(any(w & 0xFFFF == child_id for w in slots), 'host sees the child %#x: %s' % (child_id, [hex(w) for w in slots]))
    finally:
        if relay:
            relay.close()
        a.close()
        b.close()


def join(ga, gb):
    """host_room on ga, find_room + connect on gb; returns the child slot."""
    host_room(ga)
    found = find_room(gb)
    gb.ok(SP_END)
    gb.ok(CP_START, struct.pack('<H', found[0] & 0xFFFF))
    end = time.time() + 4
    while True:
        st = gb.ok(CP_POLL)[0]
        if st != CONN_INPROGRESS or time.time() > end:
            break
        time.sleep(0.1)
    check(st & 0xFF000000 == 0, 'connected: %#x' % st)
    gb.ok(CP_END)
    return (st >> 16) & 0xFF


def wait_data_event(g, timeout=3):
    """MS_Change until the adapter delivers a data event (0x28)."""
    end = time.time() + timeout
    while time.time() < end:
        mark = len(g.s.events)
        g.ok(MS_CHANGE)
        cmd, words = g.slave_event(mark, timeout=3)
        if cmd == 0x28 and words[0] & 0xFF00 == 0:   # not the 1-word "no response"
            return words
    raise AssertionError('no data event')


@test
def rfu_data_both_ways():
    """Payloads through both adapters and the relay, each way: the sender's
    DataTx, then the receiver's wait ending in a data event the adapter clocks
    in as master, then its DataRx -- gpsp's byte-count layouts on both sides."""
    a, b = Sim(), Sim()
    relay = None
    try:
        ga, gb = enter_wireless(a), enter_wireless(b)
        check(ga.check_id() == 0x8001 and gb.check_id() == 0x8001, 'both adapters found')
        relay = Relay(a, b)
        slot = join(ga, gb)

        # A game reads a frame per data event and the host's adapter also
        # sends its idle carrier, so read until the payload comes through.
        down = b'PARENT->CHILD123'
        ga.ok(DATA_TX, struct.pack('<I', len(down)) + down)
        got = []
        for _ in range(10):
            wait_data_event(gb)
            rx = gb.ok(DATA_RX)
            got.append(rx[0])
            if rx[0] == len(down):
                break
        check(rx[0] == len(down), 'child receives %d bytes: frames of %s' % (len(down), got))
        check(struct.pack('<%dI' % (len(rx) - 1), *rx[1:])[:len(down)] == down, 'child payload')

        up = b'CHILD->PARENT!'
        gb.ok(DATA_TX, struct.pack('<I', len(up) << (8 + slot * 5)) + up)
        got = []
        for _ in range(10):
            wait_data_event(ga)
            rx = ga.ok(DATA_RX)
            n = (rx[0] >> (8 + slot * 5)) & 0x1F
            got.append(n)
            if n == len(up):
                break
        check(n == len(up), 'parent receives %d bytes from slot %d: frames of %s' % (len(up), slot, got))
        check(struct.pack('<%dI' % (len(rx) - 1), *rx[1:])[:n] == up, 'parent payload')
    finally:
        if relay:
            relay.close()
        a.close()
        b.close()


def game_api(sim, phase_ok, timeout):
    """The HOST/JOIN scenario's events (rfu_game.cpp, kind 8) once it ends."""
    end = time.time() + timeout
    while time.time() < end:
        with sim.cv:
            evs = [e for c, e in sim.events if c == EV_RFU and e[0] in (0, 8)]
        if any(e[0] == 0 for e in evs):
            raise Skip('librfu not built in (test/gblink_sim/fetch_librfu.sh)')
        if any(e[1] in (5, 0xFF) for e in evs):
            return evs
        time.sleep(0.1)
    raise AssertionError('scenario did not finish: %s' % [e.hex() for e in evs])


def game_api_session(a, b, frames=120):
    """librfu's game API (librfu_rfu.c) on both GBAs, the way FireRed's link
    code drives it: rfu_initializeAPI, checkID, reset, configure; the host
    searches for a child, the other searches for the host's game name and
    connects; then `frames` frames of UNI data each way through recvData /
    readySendData / sendData. Returns (host result, join result)."""
    gname = b'GBAGATE TEST\0'
    a.gba(0x30, *gname, *b'BROCK\0\0\0', *struct.pack('<H', frames))
    time.sleep(0.3)
    b.gba(0x31, *gname, *struct.pack('<H', frames))
    ha = game_api(a, None, 40)
    hb = game_api(b, None, 40)
    def outcome(evs):
        failed = [e for e in evs if e[1] == 0xFF]
        if failed:
            return 'failed at step %d (err %#x)' % (failed[0][2], failed[0][3] | (failed[0][4] << 8))
        return 'phases %s' % sorted({e[1] for e in evs})
    for name, evs in (('host', ha), ('join', hb)):
        # Both sides in the message: one side's failure is usually the other's
        # doing (a host with no child is a join that never connected).
        check(not any(e[1] == 0xFF for e in evs), '%s %s; host %s, join %s' % (
            name, outcome(evs), outcome(ha), outcome(hb)))
    found = [e for e in hb if e[1] == 3][0]
    check(found[4:16] == gname[:12], 'the joining game found the host room by name: %r' % found[4:17])
    check(found[17:22] == b'BROCK', 'with the host user name: %r' % found[17:25])
    check(any(e[1] == 2 for e in ha), 'the host saw a child join')
    res = []
    for evs in (ha, hb):
        n, good, bad = struct.unpack('<3H', [e for e in evs if e[1] == 5][0][2:8])
        res.append((n, good, bad))
    return res


@test
def rfu_game_api_host_and_join():
    """librfu's game-facing layer over two adapters and a relay: room found by
    name, connected, and UNI frames delivered in order both ways."""
    a, b = Sim(), Sim()
    relay = None
    try:
        enter_wireless(a)
        enter_wireless(b)
        relay = Relay(a, b)
        (n, hg, hbad), (_, jg, jbad) = game_api_session(a, b)
        check(hbad == 0 and jbad == 0, 'no frame out of order or foreign: host %d, join %d' % (hbad, jbad))
        check(hg >= n * 9 // 10 and jg >= n * 9 // 10,
              'frames delivered each way: host got %d/%d, join %d/%d' % (hg, n, jg, n))
    finally:
        if relay:
            relay.close()
        a.close()
        b.close()


def main(argv):
    pick = argv[argv.index('-k') + 1] if '-k' in argv else ''
    fails = skips = ran = 0
    for t in TESTS:
        if pick and pick not in t.__name__:
            continue
        ran += 1
        t0 = time.time()
        try:
            t()
            print('  ok    %-40s %5.2fs' % (t.__name__, time.time() - t0))
        except Skip as e:
            skips += 1
            print('  skip  %-40s %s' % (t.__name__, e))
        except Exception:
            fails += 1
            print('  FAIL  %s' % t.__name__)
            traceback.print_exc()
    if fails:
        verdict = '%d of %d FAILED' % (fails, ran)
    elif skips:
        verdict = '%d passed, %d skipped (no librfu)' % (ran - skips, skips)
    else:
        verdict = 'all %d checks passed' % ran
    print('rfu            %s' % verdict)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
