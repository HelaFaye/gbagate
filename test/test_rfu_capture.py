#!/usr/bin/env python3
"""test_rfu_capture.py -- tools/rfu_capture.py and tools/rfu_rf_analyze, end to end.

A stand-in ESP-SDR on a pty answers the documented protocol (SYNC, INFO,
LIMITS?, VERSION?, FREQ, BANDWIDTH, GAIN, CAP20 -> DATA + packed samples,
RELEASE) with a synthetic AGB-015-like burst: AM at 60% depth, Manchester at
5 Mbit/s, 80 MS/s, a carrier offset and noise, sent as (I, -Q). Checked: the
capture script saves the payload untouched with its record, marks a payload
whose CRC does not match (and keeps it), sweeps the gains asked for; and the
analyzer finds the burst in the saved file and decodes the bits that were
sent.

    python3 test/test_rfu_capture.py build/rfu_rf_analyze
"""
import json
import math
import os
import pty
import random
import subprocess
import sys
import tempfile
import threading
import tty
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rfu_capture  # noqa: E402

fails = 0


def check(what, ok, extra=""):
    global fails
    print("  %s  %s%s" % ("ok  " if ok else "FAIL", what, (" (%s)" % extra) if extra and not ok else ""))
    if not ok:
        fails += 1


PATTERN = bytes([0x5A, 0xC3, 0x0F, 0x96, 0x21, 0xEE, 0x48, 0xB7] * 4)    # 256 bits


def burst(n=16380, fs=80e6, rate=5e6, depth=0.6, scale=300, noise=10, cfo=120e3, seed=5):
    """(I, Q) of a burst: 2 us of noise, 8 bits of carrier, PATTERN, 4 bits of carrier."""
    rnd = random.Random(seed)
    th = fs / (2 * rate)
    halves = [1, 1] * 8
    for k in range(len(PATTERN) * 8):
        b = PATTERN[k // 8] >> (7 - k % 8) & 1
        halves += [0, 1] if b else [1, 0]                   # IEEE: 0 high-low, 1 low-high
    halves += [1, 1] * 4
    start = int(2e-6 * fs)
    amp, y = [], 0.0
    for k in range(n):
        h = int((k - start) / th) if k >= start else -1
        target = 0.0 if h < 0 or h >= len(halves) else (1.0 if halves[h] else 1 - depth)
        y += 0.6 * (target - y)                             # a few samples of rise
        amp.append(y)
    iq = []
    for k, a in enumerate(amp):
        ph = 2 * math.pi * cfo * k / fs
        iq.append((round(a * scale * math.cos(ph) + rnd.gauss(0, noise)),
                   round(a * scale * math.sin(ph) + rnd.gauss(0, noise))))
    return iq


def pack_cap20(iq):
    """ESP-SDR's CAP20: 20 bits a sample, LSB first, I then -Q, signed 10-bit."""
    acc, nbits, out = 0, 0, bytearray()
    for i, q in iq:
        i = max(-512, min(511, i)) & 0x3FF
        mq = max(-512, min(511, -q)) & 0x3FF
        acc |= (i | mq << 10) << nbits
        nbits += 20
        while nbits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nbits -= 8
    if nbits:
        out.append(acc & 0xFF)
    return bytes(out)


class FakeSdr(threading.Thread):
    """ESP-SDR's side of the serial protocol, on the master end of a pty."""

    def __init__(self, fd, payload, corrupt_second=False):
        super().__init__(daemon=True)
        self.fd, self.payload, self.corrupt_second = fd, payload, corrupt_second
        self.log, self.caps = [], 0

    def reply(self, s):
        os.write(self.fd, (s + "\n").encode())

    def run(self):
        buf = b""
        os.write(self.fd, b"boot noise\r\nmore noise\n")    # what a reset leaves
        while True:
            try:
                chunk = os.read(self.fd, 4096)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                cmd = line.decode().strip()
                self.log.append(cmd)
                if cmd.startswith("SYNC "):
                    self.reply(cmd)
                elif cmd == "INFO":
                    self.reply("S3SDR 6 burst 16380")
                elif cmd == "LIMITS?":
                    self.reply('LIMITS {"gain":[0,63,1],"bandwidth":[1,40,1,20],"rates":[0,1,6],"bits":[8,10]}')
                elif cmd == "VERSION?":
                    self.reply('VERSION {"git":"fake","built":"now"}')
                elif cmd.startswith(("FREQ", "BANDWIDTH", "GAIN")) or cmd == "RELEASE":
                    self.reply("OK")
                elif cmd.startswith("CAP20 "):
                    n = int(cmd.split()[1])
                    data = self.payload[: (n * 20 + 7) // 8]
                    crc = zlib.crc32(data) & 0xFFFFFFFF
                    self.caps += 1
                    if self.corrupt_second and self.caps == 2:
                        data = bytes([data[0] ^ 0xFF]) + data[1:]    # damaged in transit
                    self.reply("DATA %d %08x 204.7" % (n, crc))
                    os.write(self.fd, data)
                else:
                    self.reply("ERR unknown")


def main():
    analyze = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "rfu_rf_analyze")
    print("rfu_capture: ESP-SDR protocol, records, CRC")
    iq = burst()
    payload = pack_cap20(iq)
    master, slave = pty.openpty()
    tty.setraw(slave)            # a pty echoes by default; a serial port does not
    fake = FakeSdr(master, payload, corrupt_second=True)
    fake.start()
    out = tempfile.mkdtemp(prefix="rfucap-")
    rc = rfu_capture.main(["--port", os.ttyname(slave), "--out", out, "--label", "exchange",
                           "--count", "2", "--gain", "12", "20", "--board", "test-s3"])
    os.close(slave)
    check("a damaged payload makes the run fail (exit 1)", rc == 1, rc)
    check("boot noise discarded: SYNC first, then INFO, LIMITS?, VERSION?, FREQ 2432, BANDWIDTH 20",
          fake.log[:6] == [fake.log[0], "INFO", "LIMITS?", "VERSION?", "FREQ 2432", "BANDWIDTH 20"]
          and fake.log[0].startswith("SYNC "), fake.log[:6])
    check("each gain set before its snapshots, then RELEASE",
          [c for c in fake.log if c.startswith(("GAIN", "CAP20", "RELEASE"))] ==
          ["GAIN MANUAL 12", "CAP20 16380 0", "CAP20 16380 0", "GAIN MANUAL 20", "CAP20 16380 0",
           "CAP20 16380 0", "RELEASE"], fake.log)
    files = sorted(os.listdir(out))
    check("four snapshots, each a payload and a record", len(files) == 8, files)
    with open(os.path.join(out, "exchange-000-g12.cap20"), "rb") as f:
        saved = f.read()
    check("the payload is saved untouched", saved == payload[: len(saved)] and len(saved) == (16380 * 20 + 7) // 8)
    with open(os.path.join(out, "exchange-000-g12.json")) as f:
        rec = json.load(f)
    check("its record: label, board, frequency, gain, rate, CRC matched",
          rec["label"] == "exchange" and rec["board"] == "test-s3" and rec["freq_mhz"] == 2432 and
          rec["gain"] == 12 and rec["msps"] == 80 and rec["samples"] == 16380 and rec["crc_ok"] is True and
          rec["info"].startswith("S3SDR"), rec)
    with open(os.path.join(out, "exchange-001-g12.json")) as f:
        bad = json.load(f)
    check("the damaged one is kept and marked", bad["crc_ok"] is False and
          os.path.exists(os.path.join(out, "exchange-001-g12.cap20")))

    print("rfu_rf_analyze: the saved snapshot decoded")
    r = subprocess.run([analyze, os.path.join(out, "exchange-000-g12.cap20"), "--rate-code", "0"],
                       capture_output=True, text=True)
    text = r.stdout
    print("    " + "\n    ".join(text.strip().splitlines()[:5]))
    check("one burst", "  1 burst(s)" in text, text)
    check("at 5 Mbit/s", " 5.0" in text or " 4.99" in text, text)
    check("no violations, clean", " 0 violations" in text and "not a clean" not in text, text)
    # the lead-in can take the first bit (its high half joins the carrier): look for the
    # pattern, shifted by up to one bit, in the hex
    hexbits = "".join(text.split("violations", 1)[1].split()).upper() if "violations" in text else ""
    want = PATTERN.hex().upper()
    bits_got = bin(int(hexbits, 16))[2:].zfill(len(hexbits) * 4) if hexbits else ""
    bits_want = bin(int(want, 16))[2:].zfill(len(want) * 4)
    check("the bits that were sent", bits_want[2:-2] in bits_got, hexbits[:64])

    print("rfu_capture: %s" % ("all checks passed" if not fails else "%d FAILED" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
