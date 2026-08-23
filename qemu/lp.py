"""
lp.py -- the link_proto wire format, in Python.

Must agree byte for byte with src/link_proto.c. A broker that framed packets
even slightly differently would fail as "the firmware ignores everything",
which is a miserable thing to debug from the outside -- so this module is
cross-checked against the C implementation by qemu/test_lp.py.

Layout, before framing:

    [type:1][seq:2 LE][payload][crc16:2 LE]

CRC is CCITT-FALSE over type..payload. Byte streams are SLIP framed; datagrams
are not.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

VERSION = 1

# packet types, from link_proto.h
HELLO      = 0x01
HELLO_ACK  = 0x02
WORD       = 0x10
RESULT     = 0x11
UNDERRUN   = 0x12
RESET      = 0x20
PING       = 0x30
PONG       = 0x31

NAMES = {HELLO: "HELLO", HELLO_ACK: "HELLO_ACK", WORD: "WORD",
         RESULT: "RESULT", UNDERRUN: "UNDERRUN", RESET: "RESET",
         PING: "PING", PONG: "PONG"}

PAYLOAD_LEN = {
    HELLO: 3, HELLO_ACK: 3, WORD: 2, RESULT: 10,
    UNDERRUN: 0, RESET: 0, PING: 4, PONG: 4,
}

ROLE_SLAVE, ROLE_MASTER = 0, 1
WORD_EMPTY = 0xFFFF

SLIP_END, SLIP_ESC, SLIP_ESC_END, SLIP_ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE. Matches lp_crc16() in link_proto.c."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


@dataclass
class Packet:
    type: int
    seq: int = 0
    # only the field matching `type` is meaningful
    version: int = VERSION
    role: int = ROLE_MASTER
    players: int = 2
    word: int = 0
    words: list = field(default_factory=lambda: [WORD_EMPTY] * 4)
    self_id: int = 0
    stamp_us: int = 0

    def payload(self) -> bytes:
        if self.type in (HELLO, HELLO_ACK):
            return bytes([self.version, self.role, self.players])
        if self.type == WORD:
            return struct.pack("<H", self.word & 0xFFFF)
        if self.type == RESULT:
            body = b"".join(struct.pack("<H", w & 0xFFFF) for w in self.words[:4])
            return body + bytes([self.players, self.self_id])
        if self.type in (PING, PONG):
            return struct.pack("<I", self.stamp_us & 0xFFFFFFFF)
        return b""

    def encode_raw(self) -> bytes:
        body = bytes([self.type]) + struct.pack("<H", self.seq & 0xFFFF) + self.payload()
        return body + struct.pack("<H", crc16(body))

    def encode(self) -> bytes:
        """SLIP framed, with a leading END so a partial frame at the far end is
        flushed rather than merged with this one."""
        out = bytearray([SLIP_END])
        for b in self.encode_raw():
            if b == SLIP_END:
                out += bytes([SLIP_ESC, SLIP_ESC_END])
            elif b == SLIP_ESC:
                out += bytes([SLIP_ESC, SLIP_ESC_ESC])
            else:
                out.append(b)
        out.append(SLIP_END)
        return bytes(out)

    def __str__(self) -> str:
        n = NAMES.get(self.type, "0x%02X" % self.type)
        if self.type == WORD:
            return f"{n} seq={self.seq} word={self.word:04X}"
        if self.type == RESULT:
            ws = " ".join(f"{w:04X}" for w in self.words)
            return f"{n} seq={self.seq} [{ws}] players={self.players}"
        if self.type in (HELLO, HELLO_ACK):
            return f"{n} v{self.version} role={'master' if self.role else 'slave'}"
        return f"{n} seq={self.seq}"


def decode_raw(buf: bytes) -> Packet | None:
    if len(buf) < 5:
        return None
    ptype = buf[0]
    plen = PAYLOAD_LEN.get(ptype)
    if plen is None or len(buf) != 3 + plen + 2:
        return None
    if struct.unpack_from("<H", buf, 3 + plen)[0] != crc16(buf[:3 + plen]):
        return None

    p = Packet(type=ptype, seq=struct.unpack_from("<H", buf, 1)[0])
    body = buf[3:3 + plen]
    if ptype in (HELLO, HELLO_ACK):
        p.version, p.role, p.players = body[0], body[1], body[2]
    elif ptype == WORD:
        p.word = struct.unpack_from("<H", body, 0)[0]
    elif ptype == RESULT:
        p.words = list(struct.unpack_from("<4H", body, 0))
        p.players, p.self_id = body[8], body[9]
    elif ptype in (PING, PONG):
        p.stamp_us = struct.unpack_from("<I", body, 0)[0]
    return p


class Decoder:
    """Incremental SLIP receiver. Mirrors lp_rx_t."""

    MAX = 24

    def __init__(self):
        self.buf = bytearray()
        self.escaped = False
        self.overflow = False
        self.dropped = 0

    def feed(self, data: bytes):
        """Yields Packet objects as they complete."""
        for b in data:
            if b == SLIP_END:
                if self.buf and not self.overflow:
                    pkt = decode_raw(bytes(self.buf))
                    if pkt is not None:
                        yield pkt
                    else:
                        self.dropped += 1
                elif self.overflow:
                    self.dropped += 1
                self.buf.clear()
                self.escaped = False
                self.overflow = False
                continue

            if b == SLIP_ESC:
                self.escaped = True
                continue

            if self.escaped:
                self.escaped = False
                if b == SLIP_ESC_END:
                    b = SLIP_END
                elif b == SLIP_ESC_ESC:
                    b = SLIP_ESC
                else:
                    self.overflow = True
                    continue

            if len(self.buf) >= self.MAX:
                self.overflow = True
                continue
            self.buf.append(b)
