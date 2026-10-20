"""test_gblink.py -- the GB-Link personality, end to end, on a host.

Each test starts build/gblink_sim (GB-Link's own firmware logic on the gbagate
shim) and plays the host's part byte for byte as the GB-Link apps do. The
command sequences are taken from those apps, cited per test, so a failure here
means a real GB-Link host would fail the same way.

    python3 -I test/gblink_sim/test_gblink.py          run everything
    python3 -I test/gblink_sim/test_gblink.py -k cable only matching tests
"""
import os
import struct
import subprocess
import sys
import tempfile
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simdrv import (eventually, run_tests, Sim, SIM, ST, EV_VCC, EV_LED, EV_REBOOT, EV_BOOTLOADER,  # noqa: E402
                    EV_FLASH, EV_PHY, EV_GB)

CHILD, PARENT, ECHO = 1, 2, 3
TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def check(cond, what):
    if not cond:
        raise AssertionError(what)


def words16(buf):
    return list(struct.unpack('<%dH' % (len(buf) // 2), buf))


def link_up_master(s, grace=0.15):
    """netplay-bridge adapter.ts initLinkMode('master') + startHandshake +
    connectLink, with linkBridge.ts's CONNECT_GRACE_MS between the last two."""
    s.cmd(0x01)
    time.sleep(0.05)
    mark = len(s.status)
    s.cmd(0x00, 0x01)
    s.wait_status(ST['DeviceReady'], start=mark)
    s.wait_status(ST['AwaitMode'], start=mark)
    s.cmd(0x10)
    s.wait_status(ST['HandshakeReceived'], 5, start=mark)
    s.cmd(0x12)
    time.sleep(grace)
    s.cmd(0x13)
    s.wait_status(ST['LinkConnected'], 5, start=mark)


# ---- device identity and settings (0x0F, 0x4x) -----------------------------

@test
def firmware_info_layout():
    """adapter.ts getFirmwareInfo: [0x0F, major, minor, patch, landing, cable]
    on the DATA endpoint, never the status one."""
    s = Sim()
    try:
        s.cmd(0x0F)
        d = s.wait_data(lambda d: d[0] == 0x0F)
        check(len(d) == 6, 'six bytes, got %d' % len(d))
        check(tuple(d[1:4]) == (2, 2, 7), 'reports the vendored protocol level 2.2.7')
        check(d[4] == 1, 'landing page defaults on')
        check(d[5] == 2, 'fresh-install cable selection is GBC (persist.cpp)')
        check(not s.status, 'no status word for a query')
    finally:
        s.close()


@test
def settings_persist_across_power_cycles():
    """launcher: 0x44 landing, 0x4B cable, 0x46 LED slot; all survive a reboot."""
    path = tempfile.mktemp()
    try:
        s = Sim(flash=path)
        s.cmd(0x44, 0x00)
        s.cmd(0x4B, 0x01)
        s.cmd(0x46, 0x03, 0x11, 0x22, 0x33)
        s.cmd(0x0F)
        d = s.wait_data(lambda d: d[0] == 0x0F)
        check(d[4] == 0 and d[5] == 1, 'applied immediately: %s' % d.hex())
        s.close()

        s = Sim(flash=path)
        s.cmd(0x0F)
        d = s.wait_data(lambda d: d[0] == 0x0F)
        check(d[4] == 0 and d[5] == 1, 'persisted: %s' % d.hex())
        s.cmd(0x45)
        led = s.wait_data(lambda d: d[0] == 0x45)
        check(len(led) == 23 and led[1] == 7, 'LED config is [45 07 rgb*7] (2.2.7 added the chip gate slot)')
        check(tuple(led[2 + 18:2 + 21]) == (2, 4, 32), 'slot 6, Battle Chip Gate: deep blue: %s' % led.hex())
        check(tuple(led[2 + 9:2 + 12]) == (0x11, 0x22, 0x33), 'slot 3 kept: %s' % led.hex())
        mark = len(s.data)
        s.cmd(0x47)
        s.cmd(0x45)
        led = s.wait_data(lambda d: d[0] == 0x45, start=mark)
        check(tuple(led[2 + 9:2 + 12]) == (32, 0, 32), 'reset to purple: %s' % led.hex())
        s.close()
    finally:
        if os.path.exists(path):
            os.unlink(path)


@test
def cable_selection_values():
    """types.ts CABLE_SELECTION_VALUE; out-of-range 0x4B is ignored."""
    s = Sim()
    try:
        for v in (0, 1, 2):
            s.cmd(0x4B, v)
            mark = len(s.data)
            s.cmd(0x0F)
            d = s.wait_data(lambda d: d[0] == 0x0F, start=mark)
            check(d[5] == v, 'selection %d stored: %s' % (v, d.hex()))
        s.cmd(0x4B, 7)
        mark = len(s.data)
        s.cmd(0x0F)
        d = s.wait_data(lambda d: d[0] == 0x0F, start=mark)
        check(d[5] == 2, 'invalid selection left the last valid one: %s' % d.hex())
    finally:
        s.close()


@test
def cable_type_report():
    """ereader gblink.js: 0x4A answers [0x4A, 0 GBA | 1 GBC] from the cable
    sampled at mode entry, steered by 0x49."""
    s = Sim()
    try:
        s.gba(0x06, 0)  # GBA cable: SI hard-wired low
        s.cmd(0x4B, 0)  # auto
        s.cmd(0x00, 0x01)
        s.wait_status(ST['AwaitMode'])
        s.cmd(0x4A)
        d = s.wait_data(lambda d: d[0] == 0x4A)
        check(d[1] == 0, 'detected a GBA cable')
        s.cmd(0x49, 2)
        mark = len(s.data)
        s.cmd(0x4A)
        d = s.wait_data(lambda d: d[0] == 0x4A, start=mark)
        check(d[1] == 1, 'override to GBC reported')
    finally:
        s.close()


@test
def hardware_commands():
    s = Sim()
    try:
        s.cmd(0x41)
        check(s.wait_event(EV_VCC) == b'\x01', '5 V')
        s.cmd(0x40)
        n = len(s.events)
        check(s.wait_event(EV_VCC, start=n) == b'\x00', '3.3 V')
        s.cmd(0x42, 1, 2, 3, 1)
        check(s.wait_event(EV_LED, lambda b: b == b'\x01\x02\x03\x01'), 'live LED')
        s.cmd(0x48)
        s.wait_event(EV_REBOOT)
        s.cmd(0x43)
        s.wait_event(EV_BOOTLOADER)
    finally:
        s.close()


@test
def malformed_commands_are_ignored():
    """Truncated commands must not crash or act: every handler checks length."""
    s = Sim()
    try:
        for c in ([0x00], [0x42, 1], [0x46, 1, 2], [0x49], [0x4B], [0x30, 1], [0x5A], [0xFF]):
            s.cmd(*c)
        s.cmd(0x0F)
        s.wait_data(lambda d: d[0] == 0x0F)
        check(s.p.poll() is None, 'still running')
    finally:
        s.close()


# ---- mode 1: GBA link relay -------------------------------------------------

@test
def relay_master_full_session():
    """netplay-bridge LinkBridge in 'join': adapter MASTER, real GBA as child."""
    s = Sim()
    try:
        s.gba(0x01, CHILD)
        link_up_master(s)
        expect = [ST['DeviceReady'], ST['AwaitMode'], ST['HandshakeReceived'], ST['LinkConnected']]
        check(s.status[-4:] == expect, 'status order %s' % [hex(x) for x in s.status])

        # Host -> GBA: one 64-byte packet is four frames of eight words.
        frames = [[0x2222 + i, 0x1111, i, 0, 0, 0, 0, 0] for i in range(4)]
        s.write_data(b''.join(struct.pack('<8H', *f) for f in frames))
        # GBA -> host: a frame the GBA sends arrives in a 64-byte packet.
        s.gba(0x02, *struct.pack('<8H', 0xBBBB, 0x0064, 0x0080, 0, 0, 0, 0, 0))
        d = s.wait_data(lambda d: len(d) == 64 and 0xBBBB in words16(d), 5)
        check(len(d) == 64, 'relay data is always 64 bytes')

        eventually(lambda: all(f[0] in [tx for tx, rx in s.words()] for f in frames), 5,
                   lambda: 'every frame reached the GBA: %s' % [hex(f[0]) for f in frames
                                                            if f[0] not in [tx for tx, rx in s.words()]])
    finally:
        s.close()


@test
def relay_idle_keys_are_thinned():
    """usbSection.cpp: after the second consecutive CAFE/0x11, the command is
    zeroed, and an all-zero packet is not sent at all."""
    s = Sim()
    try:
        s.gba(0x01, CHILD)
        link_up_master(s)
        time.sleep(1.0)
        pk = [words16(d) for d in s.data if len(d) == 64]
        check(pk, 'some packets')
        cafe = sum(1 for p in pk for i in range(0, 32, 8) if p[i] == 0xCAFE)
        check(cafe <= 2, 'idle CAFE frames thinned (%d)' % cafe)
        check(all(any(p) for p in pk), 'no all-zero packet was sent')
    finally:
        s.close()


@test
def relay_slave_with_gba_parent():
    """netplay-bridge 'host': adapter SLAVE, the GBA is parent and decides
    when to send 0x8FFF; the host only arms the handshake."""
    s = Sim()
    try:
        s.gba(0x06, 1)
        s.gba(0x01, PARENT)
        s.gba(0x03, 4)
        s.cmd(0x00, 0x01)
        s.wait_status(ST['AwaitMode'])
        s.cmd(0x11)
        s.wait_status(ST['HandshakeReceived'], 5)
        s.cmd(0x12)
        s.wait_status(ST['LinkConnected'], 5)
        s.wait_data(lambda d: len(d) == 64, 3)
    finally:
        s.close()


@test
def relay_close_and_reconnect():
    """Both sides sending READY_CLOSE_LINK ends the section; without an
    exit-room key the module reports LinkReconnecting and re-arms."""
    s = Sim()
    try:
        s.gba(0x01, CHILD)
        link_up_master(s)
        close = struct.pack('<8H', 0x5FFF, 0, 0, 0, 0, 0, 0, 0)
        s.gba(0x02, *close)
        s.write_data(close * 4)
        s.wait_status(ST['LinkReconnecting'], 5)
        s.wait_status(ST['HandshakeReceived'], 5, start=s.status.index(ST['LinkReconnecting']))
    finally:
        s.close()


@test
def relay_exit_room_closes():
    """SEND_HELD_KEYS + EXIT_ROOM (0xCAFE, 0x17) then close: LinkClosed."""
    s = Sim()
    try:
        s.gba(0x01, CHILD)
        link_up_master(s)
        s.gba(0x02, *struct.pack('<8H', 0xCAFE, 0x17, 0, 0, 0, 0, 0, 0))
        close = struct.pack('<8H', 0x5FFF, 0, 0, 0, 0, 0, 0, 0)
        s.gba(0x02, *close)
        s.write_data(close * 4)
        s.wait_status(ST['LinkClosed'], 5)
    finally:
        s.close()


@test
def relay_connect_without_grace():
    """PATCHES.md P3: a host that sends 0x13 straight after 0x12 used to hang
    the firmware; it now links, after enough 0xB9A0 rounds for the GBA."""
    s = Sim()
    try:
        s.gba(0x01, CHILD)
        link_up_master(s, grace=0)
        w = s.words()
        i = next(k for k, (tx, rx) in enumerate(w) if tx == 0x8FFF)
        check(sum(1 for tx, rx in w[:i] if tx == 0xB9A0) >= 8, 'connect held for the GBA')
    finally:
        s.close()


@test
def relay_role_command_before_mode_role():
    """PATCHES.md P2: 0x12/0x13 before 0x10/0x11 dereferenced null."""
    s = Sim()
    try:
        s.cmd(0x00, 0x01)
        s.wait_status(ST['AwaitMode'])
        s.cmd(0x12)
        s.cmd(0x13)
        time.sleep(0.2)
        check(s.p.poll() is None, 'survived')
    finally:
        s.close()


@test
def relay_short_packet():
    """PATCHES.md P1: a short data packet is zero-padded, not padded with
    whatever the previous packet left behind."""
    s = Sim()
    try:
        s.gba(0x01, CHILD)
        link_up_master(s)
        s.write_data(struct.pack('<8H', 0x4444, 0x1234, 0, 0, 0, 0, 0, 0) * 4)
        eventually(lambda: [tx for tx, rx in s.words()].count(0x4444) >= 4, 5, 'the four frames went out')
        s.write_data(struct.pack('<8H', 0x7777, 0, 0, 0, 0, 0, 0, 0))
        eventually(lambda: 0x7777 in [tx for tx, rx in s.words()], 5, 'short packet delivered')
        time.sleep(0.3)   # a replay of the old frames would ride with it
        sent = [tx for tx, rx in s.words()]
        check(sent.count(0x4444) == 4, 'old frames not replayed (%d)' % sent.count(0x4444))
        check(0x7777 in sent, 'short packet delivered')
    finally:
        s.close()


@test
def wrong_pin_watchdog_recovers():
    """linkWrongPinWatchdog.c: SC toggling with no words means the wrong SD
    path; after ~2.2 s the firmware flips it once and the link comes up."""
    s = Sim()
    try:
        s.gba(0x06, 0)          # GBA cable...
        s.gba(0x07, 1)          # ...but SD actually on the other path
        s.gba(0x01, PARENT)
        s.cmd(0x4B, 1)          # host pinned GBA: the wrong answer
        s.cmd(0x00, 0x01)
        s.wait_status(ST['AwaitMode'])
        s.cmd(0x11)
        s.wait_event(EV_PHY, lambda b: b == bytes([2, 1]), 6)
        s.wait_status(ST['HandshakeReceived'], 3)
    finally:
        s.close()


@test
def relay_over_webserial():
    """gb-pokemon-web SerialConnection / launcher serial-transport.js: the same
    session framed 'GB' over CDC; replies come back on CDC, not USB."""
    s = Sim(serial=True)
    try:
        s.gba(0x01, CHILD)
        s.cmd(0x0F)
        d = s.wait_data(lambda d: d[0] == 0x0F)
        check(len(d) == 6, 'info over serial')
        link_up_master(s)
        s.wait_data(lambda d: len(d) == 64, 3)
    finally:
        s.close()


@test
def serial_framing_resyncs():
    """serialLayer.cpp: garbage, a 'G' run and an oversized header must not
    stop the next good frame."""
    s = Sim(serial=True)
    try:
        s.raw_cdc(b'AT+CGMI\r\nGGGB\x00\xff\xff' + b'GGB\x00\x01\x00\x0f')
        d = s.wait_data(lambda d: d[0] == 0x0F)
        check(len(d) == 6, 'synced')
    finally:
        s.close()


@test
def modem_probe_does_not_steal_transport():
    """A frame on an unknown channel must not move replies to the serial port
    (ModemManager pokes every new ttyACM)."""
    s = Sim()
    try:
        s.raw_cdc(b'GB\x07\x01\x00\x0f')
        s.cmd(0x0F)
        s.wait_data(lambda d: d[0] == 0x0F)
        check(not s.cdc, 'reply went to USB')
    finally:
        s.close()


@test
def setmode_preempts_running_mode():
    s = Sim()
    try:
        s.cmd(0x00, 0x01)
        s.wait_status(ST['AwaitMode'])
        mark = len(s.status)
        s.cmd(0x00, 0x02)
        s.wait_status(ST['LinkClosed'], start=mark)
        s.wait_status(ST['GBModeActive'], start=mark)
    finally:
        s.close()


# ---- mode 2: Game Boy SPI ---------------------------------------------------

@test
def gb_spi_passthrough():
    """gb-pokemon-web / distributor: [00 02], then each data packet is clocked
    out and answered with a packet of the same length."""
    s = Sim()
    try:
        s.cmd(0x00, 0x02)
        s.wait_status(ST['GBModeActive'])
        s.cmd(0x30, 0x00, 0x00, 0x00, 0x01)  # no gap, 1 byte per chunk
        s.gba(0x04, 0x02, 0x61, 0xD1)
        s.write_data(b'\x01\x60\xD0')
        d = s.wait_data(lambda d: True)
        check(d == b'\x02\x61\xD1', 'reply %s' % d.hex())
    finally:
        s.close()


@test
def gb_spi_timing_and_chunks():
    """Multiboot path (Multiboot.js): [30 24 00 00 04] = 36 us after every
    4-byte chunk; 56 bytes in, 56 bytes back."""
    s = Sim()
    try:
        s.cmd(0x40)
        s.cmd(0x00, 0x02)
        s.wait_status(ST['GBModeActive'])
        s.cmd(0x30, 0x24, 0x00, 0x00, 0x04)
        payload = bytes(range(56))
        mark = len(s.events)
        s.write_data(payload)
        d = s.wait_data(lambda d: True)
        check(len(d) == 56, '56 back')
        with s.cv:
            sent = [b for c, b in s.events[mark:] if c == EV_GB and len(b) == 6]
        check([b[0] for b in sent] == list(payload), 'all 56 bytes clocked, in order')
        t = [struct.unpack('<I', b[2:6])[0] for b in sent]
        gaps = [(t[i + 1] - t[i]) & 0xFFFFFFFF for i in range(3, 55, 4)]
        check(len(gaps) == 13 and min(gaps) >= 36, '36 us after every 4-byte chunk: %s' % gaps)
    finally:
        s.close()


# ---- mode 3: printer ---------------------------------------------------------

def printer_packet(cmd, data=b'', compressed=0):
    body = bytes([cmd, compressed, len(data) & 0xFF, len(data) >> 8]) + data
    ck = sum(body) & 0xFFFF
    return b'\x88\x33' + body + struct.pack('<H', ck) + b'\x00\x00'


@test
def printer_stream():
    """gb.cpp printer: FF at entry, then cmd, comp, len lo/hi, data per packet,
    FE after PRINT; the device answers 0x81 then status 0x00."""
    s = Sim()
    try:
        s.cmd(0x00, 0x03)
        s.wait_status(ST['GBPrinterModeActive'])
        s.wait_data(lambda d: d[:1] == b'\xff', 2)
        tile = bytes(range(16)) * 4
        script = printer_packet(0x01) + printer_packet(0x04, tile) + printer_packet(0x02, b'\x01\x13\xe4\x40')
        s.gba(0x05, *script)
        s.wait_event(EV_GB, lambda b: b == b'', 20)
        time.sleep(0.6)
        stream = b''.join(s.data)
        check(stream[0] == 0xFF, 'start marker')
        check(b'\x04\x00\x40\x00' + tile in stream, 'data packet forwarded: ' + stream.hex())
        check(stream.endswith(b'\xfe'), 'print marker: %s' % stream[-8:].hex())
        replies = [b[1] for c, b in s.events if c == EV_GB and len(b) == 2]
        check(replies.count(0x81) == 3, 'device id per packet: %d' % replies.count(0x81))
    finally:
        s.close()


# ---- mode 4: Advance Wars ------------------------------------------------------

@test
def advance_wars_entry():
    """adapter.ts setModeCommand('aw2') = [00 04 02]; Celio's role handshake."""
    s = Sim()
    try:
        s.gba(0x01, ECHO)
        s.cmd(0x00, 0x04, 0x02)
        s.wait_status(ST['AwaitMode'])
        s.cmd(0x10)
        s.wait_status(ST['HandshakeReceived'])
        s.cmd(0x12)
        s.wait_status(ST['LinkConnected'])
        s.wait_event(EV_PHY, lambda b: b[0] == 1)
    finally:
        s.close()


# ---- mode 5: e-Reader --------------------------------------------------------------

@test
def ereader_errors():
    """ereader gblink.js ERDR frames: on a Pokemon profile, START_SCAN with no
    card loaded answers ERROR 2 (not loaded); a card past 8 KB answers ERROR 1.
    (SMA4 profiles start the handshake first and check the card later.)"""
    s = Sim()
    try:
        s.cmd(0x00, 0x05, 0x02)
        s.wait_status(ST['AwaitMode'])
        s.cmd(0x11)
        s.wait_status(ST['HandshakeReceived'])
        s.cmd(0x12)
        s.wait_status(ST['LinkConnected'], 5)
        s.write_data(b'ERDR\x02' + bytes(59))
        d = s.wait_data(lambda d: d[:5] == b'ERDR\x12', 3)
        check(d[5] == 2, 'not loaded: %s' % d[:9].hex())
        mark = len(s.data)
        s.write_data(b'ERDR\x01' + struct.pack('<IH', 8190, 16) + bytes(16) + bytes(37))
        d = s.wait_data(lambda d: d[:5] == b'ERDR\x12', 3, start=mark)
        check(d[5] == 1, 'too large: %s' % d[:9].hex())
    finally:
        s.close()


# ---- mode 0: trade emulator -------------------------------------------------------------

@test
def trade_emu_pk3_upload_any_split():
    """Celio-Client sends a 100-byte pk3 as two 50-byte writes. PATCHES.md P4:
    any other split (one 64-byte USB packet, say) used to overrun."""
    s = Sim()
    try:
        s.cmd(0x00, 0x00)
        s.wait_status(ST['DeviceReady'])
        pk = bytes(range(100))
        for chunk in (pk[:64], pk[64:] + pk[:28], pk[28:]):
            s.write_data(chunk)
        time.sleep(0.2)
        check(s.p.poll() is None, 'survived')
    finally:
        s.close()


@test
def trade_emu_party_index_bounded():
    """PATCHES.md P9: the slot a GBA asks to trade is a link word; one past
    the six slots used to read and write past the parties (ASan, alone)."""
    exe = os.path.join(os.path.dirname(SIM), 'party_check')
    r = subprocess.run([exe], capture_output=True, text=True, timeout=30)
    check(r.returncode == 0, 'party_check: %s%s' % (r.stdout, r.stderr[-800:]))


# ---- gbagate extensions (src/gbx_proto.h) ------------------------------------------

MMBN = 4


@test
def gbagate_extended_info():
    """0x4F answers GBX identity and caps on the data endpoint; a GB-Link
    stays silent, which is how a host tells the two apart."""
    s = Sim()
    try:
        s.cmd(0x4F)
        d = s.wait_data(lambda d: d[0] == 0x4F)
        check(len(d) == 16 and d[1:4] == b'GBX', 'GBX block: %s' % d.hex())
        check(d[4] == 1, 'extension version 1')
        check(tuple(d[5:8]) == (0, 4, 0), 'gbagate 0.4.0: %s' % list(d[5:8]))
        caps = struct.unpack('<I', d[8:12])[0]
        check(caps & 1, 'chip gate capability')
        # It must not disturb the upstream identity.
        s.cmd(0x0F)
        f = s.wait_data(lambda d: d[0] == 0x0F)
        check(tuple(f[1:4]) == (2, 2, 7), 'GetFirmwareInfo unchanged')
    finally:
        s.close()


@test
def gbagate_chip_gate_session():
    """Mode 0x80: the game pings, the gate answers with its ID, the game
    echoes it, and an inserted chip goes out in nine rotations."""
    s = Sim()
    try:
        s.gba(0x01, MMBN)
        s.cmd(0x00, 0x80, 4)        # MMBN6 US/EU
        s.wait_status(ST['AwaitMode'])
        s.wait_status(ST['LinkConnected'], 5)
        gate = [tx for tx, rx in s.words() if (tx & 0xFF00) == 0xFF00 and tx != 0xFFFF]
        check(gate and gate[0] == 0xFF00, 'announced as the US Beast Link Gate: %s' % gate[:1])
        s.cmd(0x50, 0x2A, 0x01)
        check(s.wait_data(lambda d: d[0] == 0x50)[1] == 1, 'chip accepted')
        mark = len(s.data)
        s.cmd(0x50, 0x2B, 0x01)
        check(s.wait_data(lambda d: d[0] == 0x50, start=mark)[1] == 0, 'second insert refused while the first goes out')
        e = s.wait_data(lambda d: d[0] == 0x54, 5)
        check(e[1:3] == b'\x2a\x01', 'CHIP_SENT 0x012A')
        # The words are events of their own, so the last may land just after
        # CHIP_SENT; give them a moment, then it must be exactly nine.
        end = time.time() + 1
        while sum(1 for tx, rx in s.words() if tx == 0x012A) < 9 and time.time() < end:
            time.sleep(0.02)
        time.sleep(0.1)
        n = sum(1 for tx, rx in s.words() if tx == 0x012A)
        check(n == 9, 'nine rotations, got %d' % n)
        s.cmd(0x52)
        st = s.wait_data(lambda d: d[0] == 0x52)
        check(st[1] == 2 and st[5] == 4, 'state ACTIVE, game 4: %s' % st.hex())
        check(struct.unpack('<I', st[6:10])[0] == 1, 'one session')
    finally:
        s.close()


@test
def gbagate_chip_gate_busy_and_drop():
    """One chip at a time, as a real gate; a game that stops clocking ends the
    session (LinkReconnecting) instead of leaving it open for ever."""
    s = Sim()
    try:
        s.gba(0x01, MMBN)
        s.cmd(0x00, 0x80, 3)
        s.wait_status(ST['LinkConnected'], 5)
        s.cmd(0x50, 0x01, 0x00)
        s.cmd(0x50, 0x02, 0x00)
        answers = eventually(lambda: (lambda a: a if len(a) >= 2 else None)([d[1] for d in s.data if d[0] == 0x50]),
                             3, 'both inserts answered')
        check(answers[:2] == [1, 0], 'second chip refused while busy: %s' % answers)
        s.gba(0x01, 0)              # the game goes away
        s.wait_status(ST['LinkReconnecting'], 3)
        s.cmd(0x01)
        s.wait_status(ST['LinkClosed'], 3)
    finally:
        s.close()


@test
def chip_gate_upstream_mode():
    """GB-Link 2.2.7's own Battle Chip Gate (mode 0x06), the protocol
    megaman.gblink.io speaks: variant 3 is the international Beast Link Gate
    (unit code 0xFF00); a chip arrives as a data packet [variant, id lo, hi]
    and goes out in the rotation's id slot; status comes back as "BCGS"."""
    s = Sim()
    try:
        s.gba(0x01, MMBN)
        s.cmd(0x00, 0x06, 3)
        s.wait_status(ST['LinkConnected'], 5)
        bcgs = s.wait_data(lambda d: d[:4] == b'BCGS', timeout=3)
        check(len(bcgs) == 64, 'status packets are 64 bytes')
        eventually(lambda: any(tx == 0xFF00 for tx, rx in s.words()), 3, 'the gate answers with unit code 0xFF00')
        mark = len(s.words())
        s.write_data(bytes([3, 0x2A, 0x01]))
        eventually(lambda: any(tx == 0x012A for tx, rx in s.words()[mark:]), 3, 'chip 0x012A goes out in the id slot')
        s.cmd(0x01)
        s.wait_status(ST['LinkClosed'], 3)
    finally:
        s.close()


@test
def bridge_uart_companion():
    """GB-Link 2.2.7's bridge UART, the way GB-Link-Switch-LDN uses it
    (firmware/common/pico_link.c): 'GB' frames on a second serial port.
    SetMode(rfuWireless) over the bridge enters mode 0x07 and the replies come
    back on the bridge, not on USB; a USB host's GetFirmwareInfo afterwards is
    answered on USB again."""
    s = Sim()
    try:
        s.bridge_send(0, [0x0F])
        end = time.time() + 2
        while time.time() < end and not any(ch == 1 and p[:1] == b'\x0f' for ch, p in s.bridge_frames):
            time.sleep(0.02)
        info = [p for ch, p in s.bridge_frames if ch == 1 and p[:1] == b'\x0f']
        check(info and tuple(info[0][1:4]) == (2, 2, 7), 'GetFirmwareInfo answered on the bridge: %s' % info)
        usb_before = len(s.data)
        s.bridge_send(0, [0x00, 0x07, 0x00])
        end = time.time() + 3
        while time.time() < end and not any(ch == 2 and p == bytes([0x02, 0xFF]) for ch, p in s.bridge_frames):
            time.sleep(0.02)
        check(any(ch == 2 and p == bytes([0x02, 0xFF]) for ch, p in s.bridge_frames), 'AwaitMode on the bridge')
        time.sleep(0.6)
        check(any(ch == 1 and p[:1] == b'\x0e' and len(p) == 16 for ch, p in s.bridge_frames),
              "the wireless mode's telemetry goes to the companion")
        check(len(s.data) == usb_before, 'nothing on USB meanwhile')
        s.cmd(0x0F)
        d = s.wait_data(lambda d: d[0] == 0x0F, start=usb_before)
        check(tuple(d[1:4]) == (2, 2, 7), 'a USB host is answered on USB')
    finally:
        s.close()


def main(argv):
    return run_tests('gblink', TESTS, argv)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
