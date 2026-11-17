#!/usr/bin/env python3
"""rfu_capture.py -- RF-0 captures of an AGB-015 with an ESP32-S3 running ESP-SDR.

doc/RFU-RF.md's first step: a spare ESP32-S3 flashed with ESPARGOS/esp-sdr
(not gbagate: it takes the S3's Wi-Fi receiver over), near an original
Wireless Adapter, saving finite CAP20 snapshots for the host to analyse
(tools/rfu_rf_analyze). This script speaks ESP-SDR's serial protocol
(docs/protocol.md: line commands, one reply line each, DATA followed by the
packed samples) and keeps every snapshot exactly as it arrived, beside a JSON
record of what was set and what came back.

    tools/rfu_capture.py --port /dev/ttyACM0 --label discovery --count 20
    tools/rfu_capture.py --port /dev/ttyACM0 --label idle --gain 10 20 30 --count 5

Each snapshot: OUT/<label>-<n>-g<gain>.cap20 (the payload, untouched) and
.json (the board's INFO and VERSION, frequency, bandwidth, gain, rate code
and MS/s, sample count, the DATA line's CRC and microseconds, whether the
payload's CRC matched, wall-clock time, label, board and note). A snapshot
whose CRC does not match is kept and marked, not dropped.

Receive only. Nothing here transmits.
"""
import argparse
import datetime
import json
import os
import select
import sys
import termios
import time
import zlib

RATES = {0: 80, 1: 40, 2: 20, 3: 10, 4: 8, 5: 4, 6: 16, 7: 32, 8: 10.667, 9: 6.4}


class Link:
    """A raw serial port (or a pty in tests): 2 Mbaud 8N1, no flow control."""

    def __init__(self, path, baud=2000000, timeout=5.0):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        self.timeout = timeout
        self.buf = b""
        try:
            a = termios.tcgetattr(self.fd)
            a[0] = 0                                   # iflag: raw
            a[1] = 0                                   # oflag
            a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            a[3] = 0                                   # lflag: no echo, no canonical
            speed = getattr(termios, "B%d" % baud, termios.B115200)
            a[4] = a[5] = speed
            a[6][termios.VMIN] = 0
            a[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, a)
        except termios.error:
            pass                                       # not a tty: a file or socket

    def write_line(self, s):
        os.write(self.fd, (s + "\n").encode())

    def _fill(self, deadline):
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError("no reply")
        r, _, _ = select.select([self.fd], [], [], left)
        if not r:
            raise TimeoutError("no reply")
        chunk = os.read(self.fd, 65536)
        if not chunk:
            raise EOFError("port closed")
        self.buf += chunk

    def read_line(self):
        deadline = time.monotonic() + self.timeout
        while b"\n" not in self.buf:
            self._fill(deadline)
        line, self.buf = self.buf.split(b"\n", 1)
        return line.rstrip(b"\r").decode(errors="replace")

    def read_bytes(self, n):
        deadline = time.monotonic() + self.timeout + n / 150000.0   # ~2 Mbaud
        while len(self.buf) < n:
            self._fill(deadline)
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def close(self):
        os.close(self.fd)


class EspSdr:
    def __init__(self, link):
        self.link = link

    def sync(self):
        """Discard whatever a reset or an earlier client left, up to our nonce."""
        nonce = "%08x" % (int(time.time() * 1000) & 0xFFFFFFFF)
        self.link.write_line("SYNC " + nonce)
        for _ in range(64):
            if self.link.read_line().strip() == "SYNC " + nonce:
                return
        raise RuntimeError("no SYNC echo")

    def cmd(self, text):
        self.link.write_line(text)
        reply = self.link.read_line()
        if reply.startswith("ERR"):
            raise RuntimeError("%s: %s" % (text, reply))
        return reply

    def cap20(self, samples, rate):
        reply = self.cmd("CAP20 %d %d" % (samples, rate))
        parts = reply.split()
        if len(parts) != 4 or parts[0] != "DATA":
            raise RuntimeError("CAP20: unexpected reply %r" % reply)
        n, crc, us = int(parts[1]), int(parts[2], 16), float(parts[3])
        payload = self.link.read_bytes((n * 20 + 7) // 8)
        return n, crc, us, payload


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", required=True)
    ap.add_argument("--out", default="captures")
    ap.add_argument("--label", required=True, help="the adapter's state: idle, discovery, join, linked, exchange, ...")
    ap.add_argument("--count", type=int, default=1, help="snapshots per gain")
    ap.add_argument("--freq", type=int, default=2432, help="MHz (the AGB-015's nominal carrier)")
    ap.add_argument("--bandwidth", type=int, default=20, help="MHz; 0 for the widest")
    ap.add_argument("--gain", type=int, nargs="*", default=None,
                    help="manual gain indices to sweep (not dB); none: hardware AGC")
    ap.add_argument("--rate", type=int, default=0, help="rate code: 0 = 80 MS/s, 1 = 40, 6 = 16")
    ap.add_argument("--samples", type=int, default=16380)
    ap.add_argument("--board", default="", help="which S3 board, for the record")
    ap.add_argument("--note", default="")
    a = ap.parse_args(argv)

    os.makedirs(a.out, exist_ok=True)
    link = Link(a.port)
    sdr = EspSdr(link)
    failures = 0
    try:
        sdr.sync()
        info = sdr.cmd("INFO")
        limits = sdr.cmd("LIMITS?")
        try:
            version = sdr.cmd("VERSION?")
        except RuntimeError:
            version = ""
        if not info.startswith("S3SDR"):
            print("note: not an ESP32-S3 (%s)" % info, file=sys.stderr)
        try:
            rates = json.loads(limits.split(" ", 1)[1]).get("rates") or []
        except (IndexError, ValueError):
            rates = []
        if rates and a.rate not in rates and RATES.get(a.rate) not in rates:
            print("note: rate code %d is not in LIMITS? %s" % (a.rate, rates), file=sys.stderr)
        sdr.cmd("FREQ %d" % a.freq)
        sdr.cmd("BANDWIDTH %d" % a.bandwidth)
        for gain in (a.gain if a.gain else [None]):
            sdr.cmd("GAIN HARDWARE" if gain is None else "GAIN MANUAL %d" % gain)
            for i in range(a.count):
                n, crc, us, payload = sdr.cap20(a.samples, a.rate)
                ok = zlib.crc32(payload) & 0xFFFFFFFF == crc
                failures += not ok
                base = os.path.join(a.out, "%s-%03d-g%s" % (a.label, i, "agc" if gain is None else gain))
                with open(base + ".cap20", "wb") as f:
                    f.write(payload)
                rec = {
                    "label": a.label, "board": a.board, "note": a.note,
                    "time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    "info": info, "version": version, "limits": limits,
                    "freq_mhz": a.freq, "bandwidth_mhz": a.bandwidth,
                    "gain": "hardware" if gain is None else gain,
                    "rate_code": a.rate, "msps": RATES.get(a.rate),
                    "samples": n, "crc32": "%08x" % crc, "crc_ok": ok,
                    "capture_us": us, "format": "CAP20 (I, -Q) signed 10-bit, LSB first",
                }
                with open(base + ".json", "w") as f:
                    json.dump(rec, f, indent=1)
                print("%s.cap20  %d samples  %.1f us  crc %s" % (base, n, us, "ok" if ok else "MISMATCH"))
        sdr.cmd("RELEASE")
    finally:
        link.close()
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
