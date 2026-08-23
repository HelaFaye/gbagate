#!/usr/bin/env python3
"""
test_lp.py -- prove qemu/lp.py agrees with src/link_proto.c, byte for byte.

The broker sits between the firmware and mGBA. If its framing differs from the
C implementation even slightly, the symptom is "the firmware ignores
everything" -- no error, no log line, nothing to bisect. So the two are checked
against each other here rather than discovered to disagree at 2am.

Needs a C compiler. No hardware, no ESP-IDF, no QEMU.
"""

import os
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lp  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include "link_proto.h"

/* stdin: one raw (unframed) packet per line, hex. stdout: SLIP-framed hex. */
static int hexval(int c){ return c<='9'?c-'0':(c|32)-'a'+10; }

int main(void) {
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = 0;
        uint8_t raw[64];
        for (char *p = line; p[0] && p[1] && p[0] != '\n'; p += 2) {
            raw[n++] = (uint8_t)((hexval(p[0]) << 4) | hexval(p[1]));
        }
        lp_packet_t pkt;
        if (!lp_decode_raw(raw, n, &pkt)) { printf("BADPARSE\n"); continue; }

        uint8_t framed[LP_MAX_FRAMED];
        size_t f = lp_encode(&pkt, framed, sizeof framed);
        for (size_t i = 0; i < f; i++) printf("%02X", framed[i]);
        printf("\n");
    }
    return 0;
}
'''


def build_harness(tmp):
    src = os.path.join(tmp, "h.c")
    exe = os.path.join(tmp, "h")
    with open(src, "w") as fh:
        fh.write(HARNESS)
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-I", os.path.join(ROOT, "src"),
         "-o", exe, src, os.path.join(ROOT, "src", "link_proto.c")],
        check=True)
    return exe


def random_packet(rng):
    t = rng.choice([lp.HELLO, lp.HELLO_ACK, lp.WORD, lp.RESULT,
                    lp.UNDERRUN, lp.RESET, lp.PING, lp.PONG])
    p = lp.Packet(type=t, seq=rng.randrange(0x10000))
    p.version = lp.VERSION
    p.role = rng.randrange(2)
    p.players = rng.randrange(1, 5)
    p.word = rng.randrange(0x10000)
    p.words = [rng.randrange(0x10000) for _ in range(4)]
    p.self_id = rng.randrange(4)
    p.stamp_us = rng.randrange(1 << 32)
    return p


def main():
    rng = random.Random(4242)
    fails = 0

    with tempfile.TemporaryDirectory() as tmp:
        exe = build_harness(tmp)

        packets = [random_packet(rng) for _ in range(2000)]
        # plus the awkward ones: payloads containing SLIP delimiters
        packets += [
            lp.Packet(type=lp.WORD, seq=0xC0C0, word=0xDBDB),
            lp.Packet(type=lp.WORD, seq=0xDBC0, word=0xC0DB),
            lp.Packet(type=lp.PING, seq=0, stamp_us=0xC0DBC0DB),
            lp.Packet(type=lp.RESULT, seq=0xDBDB,
                      words=[0xC0C0, 0xDBDB, 0xFFFF, 0x0000]),
        ]

        stdin = "".join(p.encode_raw().hex().upper() + "\n" for p in packets)
        out = subprocess.run([exe], input=stdin, capture_output=True,
                             text=True, check=True).stdout.split()

        if len(out) != len(packets):
            print(f"  FAIL harness returned {len(out)} lines for "
                  f"{len(packets)} packets")
            return 1

        for pkt, c_hex in zip(packets, out):
            py_hex = pkt.encode().hex().upper()
            if c_hex == "BADPARSE":
                print(f"  FAIL C rejected our raw encoding of {pkt}")
                fails += 1
            elif py_hex != c_hex:
                print(f"  FAIL framing differs for {pkt}\n"
                      f"        py {py_hex}\n        c  {c_hex}")
                fails += 1

        print(f"  {len(packets)} packets encoded identically by both")

        # And the other direction: our decoder must read what C emits.
        dec = lp.Decoder()
        recovered = []
        for c_hex in out:
            if c_hex != "BADPARSE":
                recovered.extend(dec.feed(bytes.fromhex(c_hex)))
        if len(recovered) != len(packets):
            print(f"  FAIL decoded {len(recovered)} of {len(packets)}")
            fails += 1
        else:
            print(f"  {len(recovered)} packets decoded back from C's framing")

        # Corruption must be rejected, not misread.
        bad = bytearray(bytes.fromhex(out[0]))
        bad[3] ^= 0x40
        dec2 = lp.Decoder()
        if list(dec2.feed(bytes(bad))):
            print("  FAIL corrupt frame accepted")
            fails += 1
        elif dec2.dropped != 1:
            print("  FAIL corrupt frame not counted")
            fails += 1
        else:
            print("  corrupt frame rejected and counted")

    print("\n" + ("FAILURES" if fails else "lp.py agrees with link_proto.c"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
