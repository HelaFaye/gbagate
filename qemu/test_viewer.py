#!/usr/bin/env python3
"""Check viewer.py's frame parser and colour conversion against the render test."""
import os
import socket, struct, subprocess, sys, threading, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import viewer

fails = 0
def check(what, got, want):
    global fails
    if got != want:
        print(f"  FAIL {what}: got {got!r} want {want!r}"); fails += 1

# --- colour conversion must match test_render.c's save_ppm exactly ---
# save_ppm does: r=((c>>11)&0x1F)*255/31, g=((c>>5)&0x3F)*255/63, b=(c&0x1F)*255/31
for v in (0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x1234, 0xABCD):
    px = struct.pack("<H", v)
    got = viewer.rgb565_to_rgb(px, 1, 1)
    want = bytes([((v >> 11) & 0x1F) * 255 // 31,
                  ((v >> 5) & 0x3F) * 255 // 63,
                  (v & 0x1F) * 255 // 31])
    check(f"rgb565 {v:04X}", got, want)

# --- frame parsing over a real socket, with the awkward cases ---
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 0)); srv.listen(1)
port = srv.getsockname()[1]

W = H = 8
def frame(fill):
    return b"FBUF" + struct.pack("<HH", W, H) + struct.pack("<H", fill) * (W * H)

def server():
    conn, _ = srv.accept()
    # Junk first: the viewer connects mid-stream in real use.
    conn.sendall(b"\x00\xffgarbage")
    conn.sendall(frame(0xF800))
    time.sleep(0.05)
    # Split a frame across two writes.
    f = frame(0x07E0)
    conn.sendall(f[:5]); time.sleep(0.05); conn.sendall(f[5:])
    time.sleep(0.05)
    # Two frames at once: only the newest should surface.
    conn.sendall(frame(0x001F) + frame(0xFFFF))
    time.sleep(0.3)
    conn.close()

threading.Thread(target=server, daemon=True).start()
sock = socket.create_connection(("127.0.0.1", port)); sock.setblocking(False)
st = viewer.FrameStream(sock)

got = []
deadline = time.time() + 3
while time.time() < deadline and len(got) < 3:
    f = st.poll(0.05)
    if f: got.append(f[2][:2])

check("frame 1 survives leading junk", got[0] if got else None, struct.pack("<H", 0xF800))
check("frame 2 reassembled across writes", got[1] if len(got) > 1 else None, struct.pack("<H", 0x07E0))
check("coalesced: newest wins", got[2] if len(got) > 2 else None, struct.pack("<H", 0xFFFF))
check("resync counted", st.resyncs > 0, True)

# --- keymap must match what input_qemu.c parses ---
src = open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "port/esp32s3/input_qemu.c")).read()
for name, b in viewer.KEYMAP.items():
    ch = b.decode()
    lit = "'\\r'" if ch == "\r" else f"'{ch}'"
    if f"case '{ch}'" not in src and lit not in src:
        print(f"  FAIL keymap: viewer sends {name}->{ch!r}, firmware has no case")
        fails += 1
print(f"  keymap: {len(viewer.KEYMAP)} keys, all handled by input_qemu.c")

print("\n" + ("FAILURES" if fails else "viewer.py parses frames and matches the firmware"))
sys.exit(1 if fails else 0)
