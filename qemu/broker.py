#!/usr/bin/env python3
"""
broker.py -- relay between firmware in QEMU and a game.

    [ firmware in QEMU ] --UART1--> TCP :5556 <--> broker <--> :7000 [ mGBA ]

QEMU maps UART1 onto a TCP socket; the broker speaks link_proto over it, and
translates to whatever the game side wants. Three modes:

  --peer HOST:PORT   relay to another broker or another QEMU instance, so two
                     emulated devices link to each other
  --mgba PORT        listen for mGBA's Lua socket script (qemu/mgba_link.lua)
  --loop             answer the firmware ourselves: enough to prove the
                     handshake, sequencing and pacing work, with no game at all
  --gate             pretend to be a Battle Network game: send the sync word,
                     the acknowledge, then idle frames. Proves the firmware's
                     gate really answers a game's handshake, with no mGBA

Start with --loop. If the firmware will not even complete a HELLO exchange
against a broker that always answers correctly, nothing further is worth trying.

Usage:
    python3 broker.py --loop
    python3 broker.py --mgba 7000
    python3 broker.py --peer 192.168.1.20:5557
"""

from __future__ import annotations

import argparse
import os
import selectors
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lp  # noqa: E402


def now_us() -> int:
    return int(time.monotonic() * 1_000_000) & 0xFFFFFFFF


class Endpoint:
    """A socket that speaks SLIP-framed link_proto."""

    def __init__(self, sock: socket.socket, name: str):
        self.sock = sock
        self.name = name
        self.dec = lp.Decoder()
        self.sock.setblocking(False)

    def send(self, pkt: lp.Packet):
        try:
            self.sock.sendall(pkt.encode())
        except (BlockingIOError, BrokenPipeError, OSError):
            pass

    def recv(self):
        try:
            data = self.sock.recv(4096)
        except BlockingIOError:
            return []
        except OSError:
            return None
        if not data:
            return None
        return list(self.dec.feed(data))


def connect(spec: str, timeout: float = 30.0) -> socket.socket:
    host, _, port = spec.rpartition(":")
    host = host or "127.0.0.1"
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection((host, int(port)), timeout=2)
            print(f"connected to {host}:{port}")
            return s
        except OSError:
            time.sleep(0.25)
    sys.exit(f"could not connect to {host}:{port} within {timeout:.0f}s "
             f"-- is QEMU running?")


def listen(port: int) -> socket.socket:
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    print(f"waiting for a game on :{port}")
    conn, addr = srv.accept()
    print(f"game connected from {addr[0]}")
    srv.close()
    return conn


class Stats:
    def __init__(self):
        self.by_type = {}
        self.dropped = 0
        self.started = time.time()

    def count(self, pkt):
        n = lp.NAMES.get(pkt.type, "?")
        self.by_type[n] = self.by_type.get(n, 0) + 1

    def report(self):
        secs = max(time.time() - self.started, 0.001)
        parts = " ".join(f"{k}={v}" for k, v in sorted(self.by_type.items()))
        print(f"\n{secs:.1f}s  {parts}  dropped={self.dropped}")


def run_loop(fw: Endpoint, verbose: bool, stats: Stats):
    """
    Answer the firmware as a well-behaved peer would, with no game involved.
    Proves the handshake, sequencing and pacing before anything else is added.
    """
    seq = 0
    linked = False
    sel = selectors.DefaultSelector()
    sel.register(fw.sock, selectors.EVENT_READ)

    while True:
        for _ in sel.select(timeout=0.25):
            pkts = fw.recv()
            if pkts is None:
                print("firmware disconnected")
                return
            for p in pkts:
                stats.count(p)
                if verbose:
                    print(f"  fw -> {p}")

                if p.type == lp.HELLO:
                    fw.send(lp.Packet(type=lp.HELLO_ACK, seq=p.seq,
                                      role=lp.ROLE_SLAVE))
                    seq = p.seq
                    if not linked:
                        print(f"link up at seq {seq}")
                        linked = True

                elif p.type == lp.RESULT:
                    # A completed transfer. Supply our word for the next one,
                    # which is what keeps the firmware from underrunning.
                    seq = (p.seq + 1) & 0xFFFF
                    fw.send(lp.Packet(type=lp.WORD, seq=seq,
                                      word=0x0000))

                elif p.type == lp.PING:
                    fw.send(lp.Packet(type=lp.PONG, seq=p.seq,
                                      stamp_us=p.stamp_us))

                elif p.type == lp.UNDERRUN:
                    stats.dropped += 1
                    if verbose:
                        print("  (firmware underran -- we were late)")


# Gate identities, from src/chipgate.h. The game sends its own back to
# acknowledge, so the broker needs to know which one to expect.
GATE_ID = {
    "bn4":   0xFFC6,
    "bn5":   0xFFC7,
    "bn6jp": 0xFFC4,
    "bn6":   0xFF00,
}
SYNC_WORD = 0x8FFF


def run_gate(fw: Endpoint, game: str, verbose: bool, stats: Stats):
    """
    Drive the Battle Chip Gate handshake exactly as a game does.

    The game is the parent, so it decides what goes on the wire each frame:
    zeros, then the sync word, then its acknowledge, then idle. Everything the
    firmware sends back is the gate's 9-slot rotation, and seeing the gate ID
    come back is what proves the handshake completed.

    This is the check to run before involving mGBA: if the firmware will not
    answer a handshake driven by a script that gets it right every time, an
    emulator will not help.
    """
    gate_id = GATE_ID.get(game)
    if gate_id is None:
        sys.exit(f"unknown game '{game}'; expected one of {', '.join(GATE_ID)}")

    print(f"driving the {game} gate handshake, expecting id {gate_id:04X}")

    sel = selectors.DefaultSelector()
    sel.register(fw.sock, selectors.EVENT_READ)

    seq = 0
    phase = "idle"     # idle -> synced -> linked
    frames = 0
    seen_ids = set()
    rotation = []
    bridge_up = False
    kicked = False

    def seed():
        # The gate only answers a word it has received, so someone has to send
        # the first one. A real game clocks a frame ~60x a second; here a single
        # 0x0000 starts the ping-pong, and each RESULT drives the next word.
        fw.send(lp.Packet(type=lp.WORD, seq=seq, word=0x0000))

    # Bring the bridge up first.
    fw.send(lp.Packet(type=lp.HELLO, seq=seq, role=lp.ROLE_SLAVE))
    last_kick = time.monotonic()

    while True:
        events = sel.select(timeout=0.25)
        if not events:
            # No traffic yet. Until the first RESULT flows, keep prodding:
            # resend HELLO while the bridge is silent, re-seed once it is up (an
            # early word can arrive before the firmware is READY and be dropped).
            if frames == 0 and time.monotonic() - last_kick > 0.3:
                if bridge_up:
                    seed()
                else:
                    fw.send(lp.Packet(type=lp.HELLO, seq=seq,
                                      role=lp.ROLE_SLAVE))
                last_kick = time.monotonic()
            continue
        for _ in events:
            pkts = fw.recv()
            if pkts is None:
                print("firmware disconnected")
                return
            for p in pkts:
                stats.count(p)

                if p.type in (lp.HELLO, lp.HELLO_ACK):
                    if verbose:
                        print(f"  bridge up: {p}")
                    if not bridge_up:
                        bridge_up = True
                        print("bridge up; seeding the first frame")
                    if not kicked:
                        kicked = True
                        seed()
                    continue

                if p.type != lp.RESULT:
                    continue

                # word[1] is the gate's slot; word[0] is what we sent.
                answer = p.words[1]
                rotation.append(answer)
                seen_ids.add(answer)
                frames += 1

                if verbose:
                    print(f"  frame {frames:4d}  gate -> {answer:04X}")

                if phase == "idle" and answer == gate_id:
                    print(f"gate announced itself: {answer:04X}")
                    phase = "announced"

                # Now walk the handshake the way a game does.
                seq = (seq + 1) & 0xFFFF
                if phase == "announced" and frames > 2:
                    outgoing = SYNC_WORD
                    phase = "synced"
                elif phase == "synced":
                    outgoing = gate_id      # the game's acknowledge
                    phase = "linked"
                    print("sent the acknowledge; the session should open now")
                else:
                    outgoing = 0x0000

                fw.send(lp.Packet(type=lp.WORD, seq=seq, word=outgoing))

                # Once linked, the rotation should repeat every nine frames.
                if phase == "linked" and len(rotation) >= 20 and frames % 9 == 0:
                    tail = rotation[-9:]
                    print("  rotation: " + " ".join(f"{w:04X}" for w in tail))
                    if gate_id in tail and 0xFFFF in tail:
                        print("  looks like the 9-slot rotation")
                    rotation = rotation[-9:]


def run_relay(fw: Endpoint, game: Endpoint, verbose: bool, stats: Stats):
    """Straight relay. Each side's packets go to the other, unmodified."""
    sel = selectors.DefaultSelector()
    sel.register(fw.sock, selectors.EVENT_READ, (fw, game, "fw -> game"))
    sel.register(game.sock, selectors.EVENT_READ, (game, fw, "game -> fw"))

    while True:
        for key, _ in sel.select(timeout=1.0):
            src, dst, label = key.data
            pkts = src.recv()
            if pkts is None:
                print(f"{src.name} disconnected")
                return
            for p in pkts:
                stats.count(p)
                if verbose:
                    print(f"  {label}: {p}")
                dst.send(p)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qemu", default="127.0.0.1:5556",
                    help="where QEMU exposes UART1 (default 127.0.0.1:5556)")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--loop", action="store_true",
                      help="answer the firmware ourselves; no game needed")
    mode.add_argument("--gate", metavar="GAME", nargs="?", const="bn6",
                      help="drive the gate handshake as a game would "
                           "(bn4|bn5|bn6jp|bn6)")
    mode.add_argument("--mgba", type=int, metavar="PORT",
                      help="listen for mgba_link.lua on this port")
    mode.add_argument("--peer", metavar="HOST:PORT",
                      help="relay to another broker")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print every packet")
    args = ap.parse_args()

    fw = Endpoint(connect(args.qemu), "firmware")
    stats = Stats()

    try:
        if args.loop:
            print("loopback mode: answering the firmware directly")
            run_loop(fw, args.verbose, stats)
        elif args.gate:
            run_gate(fw, args.gate, args.verbose, stats)
        elif args.mgba:
            game = Endpoint(listen(args.mgba), "mGBA")
            run_relay(fw, game, args.verbose, stats)
        else:
            game = Endpoint(connect(args.peer), "peer")
            run_relay(fw, game, args.verbose, stats)
    except KeyboardInterrupt:
        pass
    finally:
        stats.report()


if __name__ == "__main__":
    main()
