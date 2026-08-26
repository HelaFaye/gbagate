#!/usr/bin/env python3
"""
fbgrab.py -- turn the QEMU framebuffer stream into a PPM.

QEMU cannot drive the real ST7735, so port/esp32s3/st7735_qemu.c streams the
firmware's framebuffer out UART2, which qemu/run.sh maps onto TCP :5557. This
reads that stream and writes a P6 PPM identical in format to the one
test/test_render.c saves -- so a screen captured from live firmware under
emulation and a screen captured from the host render test are directly
comparable.

    ./qemu/run.sh gate                       # UART2 -> :5557
    python3 qemu/fbgrab.py --one -o shot.ppm

Wire format, little-endian, one frame:

    "FBUF"  magic (4 bytes)
    width   u16
    height  u16
    pixels  width*height*2 bytes, RGB565, row major

Usage:
    python3 qemu/fbgrab.py --one                 grab one frame -> screen_qemu.ppm
    python3 qemu/fbgrab.py --one -o shot.ppm
    python3 qemu/fbgrab.py --count 5 -o run      run0.ppm .. run4.ppm
    python3 qemu/fbgrab.py --host 127.0.0.1 --port 5557 --timeout 15

A PNG is written alongside the PPM if Pillow happens to be installed; it is
never required.
"""

from __future__ import annotations

import argparse
import socket
import sys
import time

MAGIC = b"FBUF"
MAX_W = 512
MAX_H = 512


def rgb565_to_rgb888(pixels: bytes, w: int, h: int) -> bytes:
    """RGB565 (LE) -> packed RGB888, using the same rounding as test_render.c."""
    out = bytearray(w * h * 3)
    o = 0
    for i in range(w * h):
        c = pixels[2 * i] | (pixels[2 * i + 1] << 8)
        out[o]     = ((c >> 11) & 0x1F) * 255 // 31
        out[o + 1] = ((c >> 5) & 0x3F) * 255 // 63
        out[o + 2] = (c & 0x1F) * 255 // 31
        o += 3
    return bytes(out)


def ppm_bytes(pixels: bytes, w: int, h: int) -> bytes:
    """A P6 PPM, byte-for-byte the format test_render.c's save_ppm() writes."""
    header = f"P6\n{w} {h}\n255\n".encode("ascii")
    return header + rgb565_to_rgb888(pixels, w, h)


class FrameParser:
    """Incremental parser for the FBUF stream. Resynchronises on the magic, so
    a grabber that attaches mid-frame simply waits for the next whole one."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data: bytes):
        """Yield (width, height, pixels) tuples as frames complete."""
        self.buf.extend(data)
        while True:
            frame = self._take_one()
            if frame is None:
                return
            yield frame

    def _take_one(self):
        # Find the magic, discarding any junk before it.
        idx = self.buf.find(MAGIC)
        if idx < 0:
            # Keep only a possible partial magic at the tail.
            if len(self.buf) > len(MAGIC):
                del self.buf[:-len(MAGIC)]
            return None
        if idx > 0:
            del self.buf[:idx]

        if len(self.buf) < 8:
            return None
        w = self.buf[4] | (self.buf[5] << 8)
        h = self.buf[6] | (self.buf[7] << 8)
        if not (0 < w <= MAX_W and 0 < h <= MAX_H):
            # A bogus header -- skip this magic and look for the next.
            del self.buf[:len(MAGIC)]
            return None

        need = 8 + w * h * 2
        if len(self.buf) < need:
            return None
        pixels = bytes(self.buf[8:need])
        del self.buf[:need]
        return (w, h, pixels)


def write_outputs(pixels: bytes, w: int, h: int, path: str) -> None:
    with open(path, "wb") as f:
        f.write(ppm_bytes(pixels, w, h))
    print(f"wrote {path} ({w}x{h})")
    try:
        from PIL import Image  # optional, never required
        img = Image.frombytes("RGB", (w, h), rgb565_to_rgb888(pixels, w, h))
        png = path[:-4] + ".png" if path.endswith(".ppm") else path + ".png"
        img.save(png)
        print(f"wrote {png}")
    except Exception:
        pass


def grab(host: str, port: int, count: int, out: str, timeout: float) -> int:
    deadline = time.time() + timeout
    sock = None
    while time.time() < deadline and sock is None:
        try:
            sock = socket.create_connection((host, port), timeout=2)
        except OSError:
            time.sleep(0.25)
    if sock is None:
        print(f"could not connect to {host}:{port} within {timeout:.0f}s "
              f"-- is QEMU running with the UART2 serial? (see qemu/run.sh)",
              file=sys.stderr)
        return 1

    print(f"connected to {host}:{port}")
    sock.settimeout(max(1.0, deadline - time.time()))
    parser = FrameParser()
    got = 0
    try:
        while got < count and time.time() < deadline:
            try:
                data = sock.recv(65536)
            except socket.timeout:
                break
            if not data:
                break
            for w, h, pixels in parser.feed(data):
                path = (out if count == 1 and out.endswith(".ppm")
                        else f"{out}{got}.ppm")
                write_outputs(pixels, w, h, path)
                got += 1
                if got >= count:
                    break
    finally:
        sock.close()

    if got == 0:
        print("no frame received before the timeout", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5557)
    ap.add_argument("--one", action="store_true", help="grab a single frame")
    ap.add_argument("--count", type=int, default=1, help="how many frames")
    ap.add_argument("-o", "--out", default="screen_qemu.ppm",
                    help="output path (a prefix when --count > 1)")
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()

    count = 1 if args.one else args.count
    return grab(args.host, args.port, count, args.out, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
