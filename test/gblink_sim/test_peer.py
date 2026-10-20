"""test_peer.py -- the wireless link cable (src/gbl_peer.c), end to end.

Two simulated gbagate boards, each running GB-Link's adapter firmware with a
GBA behind it, and nothing between them but a simulated radio that this test
controls -- clean, lossy, duplicating. No host, no Celio: each board's
gbl_peer plays Celio's part for its own firmware.

The board with the larger nonce becomes master, so it gets the child GBA and
the other the parent GBA, as a cable between the two GBAs would.
"""
import os
import random
import struct
import sys
import threading
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simdrv import Sim, ST, eventually, run_tests  # noqa: E402

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def check(cond, what):
    if not cond:
        raise AssertionError(what)


class Air:
    """Joins two sims' radios, losing and duplicating on request."""

    def __init__(self, a, b, loss=0.0, dup=0.0, seed=1):
        self.rng = random.Random(seed)
        self.loss, self.dup = loss, dup
        self.lock = threading.Lock()
        self.sent = 0
        self.dropped = 0
        a.on_radio = lambda d: self.carry(b, d)
        b.on_radio = lambda d: self.carry(a, d)

    def carry(self, to, d):
        with self.lock:
            self.sent += 1
            if self.rng.random() < self.loss:
                self.dropped += 1
                return
            n = 2 if self.rng.random() < self.dup else 1
        for _ in range(n):
            try:
                to.radio(d)
            except (BrokenPipeError, ValueError):
                return  # the other board has been shut down


def pair(loss=0.0, dup=0.0, mode=1, variant=0, models=(1, 2)):
    a, b = Sim(), Sim()
    air = Air(a, b, loss, dup)
    a.gba(0x06, 1); b.gba(0x06, 1)
    a.gba(0x01, models[0])       # A is master: child GBA
    b.gba(0x01, models[1])       # B is slave: parent GBA
    b.gba(0x03, 6)
    a.start_peer(0x90000000, mode, variant)
    b.start_peer(0x10000000, mode, variant)
    return a, b, air


PEER_STATUS = 10


def peer_status(sim):
    with sim.cv:
        return [b[0] | (b[1] << 8) for c, b in sim.events if c == PEER_STATUS]


def wait_peer_status(sim, word, timeout, start=0):
    end = time.time() + timeout
    while time.time() < end:
        if word in peer_status(sim)[start:]:
            return
        time.sleep(0.05)
    raise TimeoutError('no %04x (saw %s)' % (word, [hex(x) for x in peer_status(sim)[start:]]))


def frame(tag):
    return struct.pack('<8H', 0xBBBB, tag, 0x0080, 0, 0, 0, 0, 0)


def exchange(a, b, timeout=20):
    """Wait for the link, then have each GBA send a tagged block frame and
    check it reaches the other GBA's wire."""
    end = time.time() + timeout
    while time.time() < end and not (len(a.words()) > 60 and len(b.words()) > 60
                                     and any(tx == 0x8FFF for tx, _ in a.words())):
        time.sleep(0.05)
    for _ in range(4):
        a.gba(0x02, *frame(0xA11A))
        b.gba(0x02, *frame(0xB22B))
    while time.time() < end:
        if any(tx == 0xB22B for tx, _ in a.words()) and any(tx == 0xA11A for tx, _ in b.words()):
            return True
        time.sleep(0.1)
    return False


@test
def wireless_cable_clean_air():
    a, b, air = pair()
    try:
        check(exchange(a, b), 'frames crossed both ways')
        check(ST['LinkConnected'] in peer_status(a) and ST['LinkConnected'] in peer_status(b), 'both linked')
        check(not a.status and not b.status, 'nothing went to USB: the cable is the host')
        # Roles as negotiated: A's adapter drove SC (master), B's answered.
        check(any(rx == 0xB9A0 for tx, rx in a.words()), 'A heard its child GBA handshake')
    finally:
        a.close(); b.close()


@test
def wireless_cable_lossy_air():
    """One datagram in five lost and one in ten duplicated: the reliable
    channel must still deliver every frame, once and in order."""
    a, b, air = pair(loss=0.2, dup=0.1)
    try:
        check(exchange(a, b, timeout=40), 'frames crossed both ways on a lossy radio')
        check(air.dropped > 0, 'the air really did drop datagrams (%d of %d)' % (air.dropped, air.sent))
        # In-order, exactly-once: each tagged frame reached the other GBA's wire
        # as many times as it was queued (4), not more. Wait for all four (the
        # retransmissions), then a little longer for any duplicate.
        eventually(lambda: [tx for tx, _ in b.words()].count(0xA11A) >= 4, 10, 'all four A->B frames arrived')
        time.sleep(1)
        got_b = [tx for tx, _ in b.words()].count(0xA11A)
        check(got_b == 4, 'A->B frames delivered exactly once each: %d' % got_b)
    finally:
        a.close(); b.close()


@test
def wireless_cable_peer_restart():
    """A board that reboots mid-session is noticed (new epoch) and both sides
    start a fresh session rather than resuming a dead one."""
    a, b, air = pair()
    try:
        check(exchange(a, b), 'first session')
        mark_a = len(peer_status(a))
        b.start_peer(0x10000000)            # B "reboots" its cable
        # Both games see the link drop and the players walk back into the
        # Cable Club: the GBAs start their handshake over.
        a.gba(0x01, 1)
        b.gba(0x01, 2)
        wait_peer_status(a, ST['LinkConnected'], 20, start=mark_a)
        check(exchange(a, b), 'second session carries frames')
    finally:
        a.close(); b.close()


@test
def wireless_cable_peer_gone():
    """The peer disappearing ends the local session after the timeout instead
    of leaving the GBA waiting on a link that cannot finish."""
    a, b, air = pair()
    try:
        check(exchange(a, b), 'linked')
        mark = len(peer_status(a))
        air.loss = 1.0                       # B falls silent
        wait_peer_status(a, ST['LinkClosed'], 8, start=mark)
    finally:
        a.close(); b.close()


@test
def wireless_adapter_over_the_cable():
    """Mode 0x07 between two boards: each runs GB-Link's Wireless Adapter
    mode, and the cable carries its RFU1 stream instead of Celio or the
    netplay bridge -- the Union Room with no PC and no Switch. The games are
    pret's librfu (see test_rfu.py)."""
    import test_rfu as T
    a, b = Sim(), Sim()
    Air(a, b)
    try:
        a.start_peer(0x90000000, 0x07, 0)
        b.start_peer(0x10000000, 0x07, 0)
        wait_peer_status(a, ST['LinkConnected'], 10)
        wait_peer_status(b, ST['LinkConnected'], 10)
        ga, gb = T.Game(a), T.Game(b)
        check(ga.check_id() == 0x8001 and gb.check_id() == 0x8001, 'both games find their adapter')
        slot = T.join(ga, gb)
        down = b'OVER THE AIR 07!'
        ga.ok(T.DATA_TX, struct.pack('<I', len(down)) + down)
        rx = [0]
        for _ in range(12):
            T.wait_data_event(gb)
            rx = gb.ok(T.DATA_RX)
            if rx[0] == len(down):
                break
        check(rx[0] == len(down) and struct.pack('<%dI' % (len(rx) - 1), *rx[1:])[:len(down)] == down,
              'host to child payload')
        up = b'AND BACK AGAIN'
        gb.ok(T.DATA_TX, struct.pack('<I', len(up) << (8 + slot * 5)) + up)
        n = 0
        for _ in range(12):
            T.wait_data_event(ga)
            rx = ga.ok(T.DATA_RX)
            n = (rx[0] >> (8 + slot * 5)) & 0x1F
            if n == len(up):
                break
        check(n == len(up) and struct.pack('<%dI' % (len(rx) - 1), *rx[1:])[:n] == up, 'child to host payload')
    finally:
        a.close()
        b.close()


@test
def wireless_adapter_game_api_over_the_cable():
    """The same with librfu's game-facing layer on both GBAs (test_rfu.py
    game_api_session): FireRed's sequence, over the air."""
    import test_rfu as T
    a, b = Sim(), Sim()
    Air(a, b)
    try:
        a.start_peer(0x90000000, 0x07, 0)
        b.start_peer(0x10000000, 0x07, 0)
        wait_peer_status(a, ST['LinkConnected'], 10)
        wait_peer_status(b, ST['LinkConnected'], 10)
        (n, hg, hbad), (_, jg, jbad) = T.game_api_session(a, b)
        check(hbad == 0 and jbad == 0, 'nothing out of order: %d %d' % (hbad, jbad))
        check(hg >= n * 9 // 10 and jg >= n * 9 // 10, 'delivered: host %d/%d, join %d/%d' % (hg, n, jg, n))
    finally:
        a.close()
        b.close()


@test
def wireless_cable_refuses_a_usb_host():
    """While the cable runs, the firmware is its host's: a PC that speaks up
    (the launcher polling GetFirmwareInfo, a stray SetMode) is refused rather
    than taking the session's replies or cancelling it. Once the cable stops,
    USB is answered again."""
    a, b, air = pair()
    try:
        check(exchange(a, b), 'linked and passing frames')
        mark = len(a.data)
        a.cmd(0x0F)                  # GetFirmwareInfo
        a.cmd(0x01)                  # and a Cancel that would end the session
        time.sleep(0.5)
        check(not any(d[:1] == b'\x0f' for d in a.data[mark:]), 'no reply on USB while the cable runs')
        before = len(b.words())
        for _ in range(4):
            a.gba(0x02, *frame(0xC33C))
        end = time.time() + 5
        while time.time() < end and not any(tx == 0xC33C for tx, _ in b.words()[before:]):
            time.sleep(0.05)
        check(any(tx == 0xC33C for tx, _ in b.words()[before:]), 'the session survived the host')
        a._send('S', 0)              # Link: Wireless left
        a.cmd(0x0F)
        d = a.wait_data(lambda d: d[0] == 0x0F, 3, start=mark)
        check(tuple(d[1:4]) == (2, 2, 7), 'answered on USB once the cable stopped')
    finally:
        a.close(); b.close()


def main(argv):
    return run_tests('peer', TESTS, argv)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
