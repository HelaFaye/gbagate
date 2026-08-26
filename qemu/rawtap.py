#!/usr/bin/env python3
"""rawtap.py -- diagnostic: dump raw bytes coming from one of the firmware's UARTs.

QEMU exposes UART1 (link_proto) on TCP :5556 and UART2 (the framebuffer stream
from port/esp32s3/st7735_qemu.c) on TCP :5557. This connects to either and
prints whatever the firmware emits, with no framing and no parsing. Its only job
is to answer one question: is the firmware putting *anything* on that UART under
emulation?

That question has to be answered before anything downstream is worth debugging,
because "the broker sees no HELLO" and "the viewer shows a blank screen" have
the same two possible causes -- the firmware is silent, or QEMU is not
forwarding that UART to the socket -- and they need completely different fixes.

  UART1 / :5556 (link_proto)
    - Bytes appear (C0 ... SLIP frames, the firmware's HELLO): UART1 forwarding
      works and the problem is in the broker/protocol layer.
    - Nothing at all: UART1 is not reaching the socket in this QEMU build, and
      the broker can never work regardless of firmware logic.

  UART2 / :5557 (framebuffer + encoder input)
    - Whole FBUF frames appear: the tap works and a blank viewer is the
      viewer's problem, not the firmware's.
    - Bytes appear but no complete frame: the stream is being truncated -- the
      most likely cause of a viewer that connects, reports no error and never
      draws, because viewer.py only paints once a *complete* frame parses.
    - Nothing at all: UART2 is not reaching the socket in this QEMU build. Both
      the screen and the encoder input (input_qemu.c reads the same socket) are
      dead, so no chip can be slotted under emulation either.

Run QEMU first (./qemu/run.sh gate), make sure nothing else is attached to the
port you are tapping, then:

  python3 qemu/rawtap.py            # taps 127.0.0.1:5556 for 8 seconds
  python3 qemu/rawtap.py 5557       # the framebuffer/UART2 side
  python3 qemu/rawtap.py 5557 15    # explicit port and seconds
"""
import socket
import sys
import time

MAGIC = b"FBUF"

# Only the framebuffer port carries FBUF frames; everything else is dumped raw.
FB_PORT = 5557
FRAME_HDR = 8

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5556
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0

is_fb = port == FB_PORT
uart = "UART2 (framebuffer)" if is_fb else "UART1 (link_proto)"

s = socket.create_connection(("127.0.0.1", port), timeout=5)
s.settimeout(0.5)
print(f"tapping 127.0.0.1:{port} for {secs:.0f}s -- raw bytes from firmware {uart}")

deadline = time.time() + secs
total = 0
buf = bytearray()
frames = 0
first_hdr = None

try:
    while time.time() < deadline:
        try:
            data = s.recv(4096)
        except socket.timeout:
            continue
        if not data:
            print("peer closed the connection")
            break
        total += len(data)

        if is_fb:
            # Count complete frames the way viewer.py does, so this agrees with
            # what the viewer would have been able to draw.
            buf.extend(data)
            while True:
                i = buf.find(MAGIC)
                if i < 0:
                    del buf[:max(0, len(buf) - 3)]
                    break
                if i:
                    del buf[:i]
                if len(buf) < FRAME_HDR:
                    break
                w = int.from_bytes(buf[4:6], "little")
                h = int.from_bytes(buf[6:8], "little")
                if first_hdr is None:
                    first_hdr = (w, h)
                if not (0 < w <= 512 and 0 < h <= 512):
                    del buf[:4]
                    continue
                need = FRAME_HDR + w * h * 2
                if len(buf) < need:
                    break
                del buf[:need]
                frames += 1
        else:
            print(" ".join(f"{b:02X}" for b in data))
finally:
    s.close()

if total == 0:
    print(f"\nNOTHING received. The firmware is not emitting on {uart} (or QEMU "
          f"is not forwarding it to the socket). This is the bug to chase.")
    if is_fb:
        print("Note: input_qemu.c reads this same socket, so the encoder input "
              "is dead too -- no chip can be slotted under emulation.")
    sys.exit(1)

if not is_fb:
    print(f"\n{total} bytes received. UART1 forwarding works; look for C0 (SLIP "
          "frame delimiters) above -- those are the firmware's HELLO frames.")
    sys.exit(0)

# Framebuffer verdict.
print(f"\n{total} bytes received, {frames} complete FBUF frame(s).")
if frames:
    w, h = first_hdr
    print(f"UART2 forwarding works and the tap is producing whole {w}x{h} frames. "
          "A blank viewer is the viewer's problem, not the firmware's.")
    sys.exit(0)

if first_hdr:
    w, h = first_hdr
    need = FRAME_HDR + w * h * 2
    print(f"Saw an FBUF header ({w}x{h}, so {need} bytes per frame) but never a "
          f"complete frame in {total} bytes. The stream is being truncated.")
else:
    print("Bytes are arriving but no FBUF magic was ever seen -- this is not the "
          "framebuffer stream, or it is corrupted beyond resynchronisation.")
print("viewer.py only draws once a complete frame parses, which is exactly why "
      "it connects, reports no error, and stays blank.")
sys.exit(1)
