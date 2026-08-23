#!/usr/bin/env python3
"""rawtap.py -- diagnostic: dump raw bytes coming from the firmware's UART1.

QEMU exposes UART1 (link_proto) on TCP :5556. This connects there and prints
whatever the firmware emits, with no framing/parsing. Its only job is to answer
one question: is the firmware putting *anything* on UART1 under emulation?

  - If bytes appear (you'll see C0 ... SLIP frames, the firmware's HELLO), then
    UART1 forwarding works and the problem is in the broker/protocol layer.
  - If nothing appears for the whole window, UART1 is not reaching the socket in
    this QEMU build, and the broker can never work regardless of firmware logic.

Run QEMU first (./qemu/run.sh gate), make sure no broker is attached, then:
  python3 qemu/rawtap.py            # taps 127.0.0.1:5556 for 8 seconds
  python3 qemu/rawtap.py 5556 15    # explicit port and seconds
"""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5556
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0

s = socket.create_connection(("127.0.0.1", port), timeout=5)
s.settimeout(0.5)
print(f"tapping 127.0.0.1:{port} for {secs:.0f}s -- raw bytes from firmware UART1")
deadline = time.time() + secs
total = 0
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
        print(" ".join(f"{b:02X}" for b in data))
finally:
    s.close()

if total == 0:
    print("\nNOTHING received. The firmware is not emitting on UART1 (or QEMU is "
          "not forwarding UART1 to the socket). This is the bug to chase, not the "
          "broker.")
else:
    print(f"\n{total} bytes received. UART1 forwarding works; look for C0 (SLIP "
          "frame delimiters) above -- those are the firmware's HELLO frames.")
