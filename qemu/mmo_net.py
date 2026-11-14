"""mmo_net.py -- the shared overworld's board <-> server messages, in Python
(src/mmo_net.h has the layout; test/test_mmo.c checks the C side).

SLIP-framed: END, type, body, CRC16-CCITT-FALSE over type and body (LE), END.
"""
import struct

END, ESC, ESC_END, ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD
HELLO, STATE, WELCOME, PLAYER, GONE = 0x01, 0x02, 0x81, 0x82, 0x83


def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


class State:
    """Where a player is: map (group, num), tile, facing, elevation, gender."""
    FMT = '<BBhhBBB'

    def __init__(self, group=0, num=0, x=0, y=0, facing=1, elevation=0, gender=0):
        self.group, self.num, self.x, self.y = group, num, x, y
        self.facing, self.elevation, self.gender = facing, elevation, gender

    def pack(self):
        return struct.pack(self.FMT, self.group, self.num, self.x, self.y, self.facing,
                           self.elevation, self.gender)

    @classmethod
    def unpack(cls, b):
        return cls(*struct.unpack(cls.FMT, b))

    def map(self):
        return (self.group, self.num)

    def key(self):
        return (self.group, self.num, self.x, self.y, self.facing, self.elevation, self.gender)

    def __eq__(self, o):
        return isinstance(o, State) and self.key() == o.key()

    def __repr__(self):
        return 'State(map=%d.%d, %d,%d, facing %d)' % (self.group, self.num, self.x, self.y, self.facing)


def encode(mtype, pid=0, state=None, version=1, maxp=16):
    if mtype == HELLO:
        body = bytes([HELLO, version])
    elif mtype == STATE:
        body = bytes([STATE]) + state.pack()
    elif mtype == WELCOME:
        body = bytes([WELCOME, pid, maxp])
    elif mtype == PLAYER:
        body = bytes([PLAYER, pid]) + state.pack()
    elif mtype == GONE:
        body = bytes([GONE, pid])
    else:
        raise ValueError(mtype)
    raw = body + struct.pack('<H', crc16(body))
    out = bytearray([END])
    for b in raw:
        if b == END:
            out += bytes([ESC, ESC_END])
        elif b == ESC:
            out += bytes([ESC, ESC_ESC])
        else:
            out.append(b)
    out.append(END)
    return bytes(out)


def decode(raw):
    """(type, fields) or None for a bad frame."""
    if len(raw) < 3 or crc16(raw[:-2]) != struct.unpack('<H', raw[-2:])[0]:
        return None
    t, b = raw[0], raw[1:-2]
    if t == HELLO and len(b) == 1:
        return t, {'version': b[0]}
    if t == STATE and len(b) == 9:
        return t, {'state': State.unpack(b)}
    if t == WELCOME and len(b) == 2:
        return t, {'pid': b[0], 'max': b[1]}
    if t == PLAYER and len(b) == 10:
        return t, {'pid': b[0], 'state': State.unpack(b[1:])}
    if t == GONE and len(b) == 1:
        return t, {'pid': b[0]}
    return None


class Reader:
    def __init__(self):
        self.buf, self.esc, self.bad = bytearray(), False, 0

    def feed(self, data):
        """Messages completed by these bytes."""
        out = []
        for b in data:
            if b == END:
                if self.buf:
                    m = decode(bytes(self.buf))
                    if m:
                        out.append(m)
                    else:
                        self.bad += 1
                self.buf, self.esc = bytearray(), False
            elif b == ESC:
                self.esc = True
            else:
                if self.esc:
                    b = END if b == ESC_END else ESC if b == ESC_ESC else b
                    self.esc = False
                self.buf.append(b)
        return out
