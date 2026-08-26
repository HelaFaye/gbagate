#!/usr/bin/env python3
"""
viewer.py -- watch the emulated firmware's screen and drive its interface.

QEMU cannot drive an ST7735, so st7735_qemu.c streams the framebuffer out a UART
and input_qemu.c reads keystrokes back on the same socket. This is the other end
of both: a live picture, and Up/Down/Enter going the other way in place of the
rotary encoder.

    ./qemu/run.sh gate                 # UART2 -> :5557
    python3 qemu/viewer.py

Which UART that is depends on how the image was built. The default is UART2, but
a QEMU whose esp32s3 machine only wires two UARTs discards everything written
there -- the socket still opens and still accepts this client, so the failure
looks exactly like a firmware that never draws. ./qemu/uart2check.sh says which
QEMU is installed. If it is two, the image is rebuilt with -D QEMU_AUX_UART=0
and the framebuffer shares UART0 with the ESP-IDF console:

    ./qemu/run.sh gate --aux-uart 0
    python3 qemu/viewer.py --console-log build/qemu-gate/console.log

Sharing works because the stream is self-delimiting: FrameStream resynchronises
on the magic and validates the geometry, so log lines between frames are skipped
rather than fatal. --console-log is what keeps them, since on that layout the
skipped bytes are the only copy of the console.

Keys
    Up / Down       one detent          (u / d to the firmware)
    PgUp / PgDn     ten detents         (U / D)
    Enter           the encoder button  (s)
    Backspace       back                (b)
    Tab             next library tab    (t)
    f               toggle favourite
    p               save the current frame as a PPM
    q / Ctrl-C      quit

Two display back ends. Tk gives a real window with nearest-neighbour scaling;
if Tk is missing -- which it often is on a server -- it falls back to drawing in
the terminal with half-block characters and 24-bit colour, which needs nothing
at all and works over ssh. Force either with --tk or --term.
"""

from __future__ import annotations

import argparse
import os
import select
import socket
import sys
import termios
import time
import tty

MAGIC = b"FBUF"
DEFAULT_W = DEFAULT_H = 128

# Key -> the byte input_qemu.c expects.
KEYMAP = {
    "up": b"u", "down": b"d",
    "pgup": b"U", "pgdn": b"D",
    "enter": b"s", "back": b"b", "tab": b"t", "fav": b"f",
}


# ---- framebuffer stream -------------------------------------------------

class FrameStream:
    """
    Reads FBUF frames off the socket.

    Resynchronises on the magic rather than assuming alignment: the firmware
    may already have been streaming before we connected, so the first bytes we
    see are usually mid-frame.

    Anything skipped past on the way to a frame is handed to `junk`, a binary
    file or None. That is not just tidiness: when the firmware is built with
    QEMU_AUX_UART=0 the console shares this socket, so the skipped bytes *are*
    the ESP-IDF log and writing them out is the only way to keep it. A whole
    frame is written by the firmware in one uart_write_bytes() call, so log
    lines land between frames rather than inside one and the two separate
    cleanly.
    """

    def __init__(self, sock: socket.socket, junk=None):
        self.sock = sock
        self.buf = bytearray()
        self.frames = 0
        self.resyncs = 0
        self.junk = junk
        self.junk_bytes = 0

    def _skip(self, data: bytes):
        """Account for bytes dropped on the way to a frame, and keep them."""
        if not data:
            return
        self.junk_bytes += len(data)
        if self.junk is not None:
            self.junk.write(data)
            # Flushed as it goes so the log can be tailed during a run rather
            # than only after the viewer exits.
            self.junk.flush()

    def poll(self, timeout: float = 0.05):
        """Return the newest complete frame, or None."""
        r, _, _ = select.select([self.sock], [], [], timeout)
        if r:
            try:
                data = self.sock.recv(65536)
            except OSError:
                return None
            if not data:
                raise ConnectionError("firmware closed the framebuffer stream")
            self.buf.extend(data)

        newest = None
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                # Keep only a magic's worth of tail, so junk cannot grow. The
                # tail is kept rather than skipped because the next read may
                # complete a magic that straddles the boundary.
                if len(self.buf) > 3:
                    self._skip(bytes(self.buf[:-3]))
                    del self.buf[:-3]
                break
            if i > 0:
                self.resyncs += 1
                self._skip(bytes(self.buf[:i]))
                del self.buf[:i]
            if len(self.buf) < 8:
                break
            w = int.from_bytes(self.buf[4:6], "little")
            h = int.from_bytes(self.buf[6:8], "little")
            if not (0 < w <= 512 and 0 < h <= 512):
                # Not a real header; step past this magic and keep looking.
                self._skip(bytes(self.buf[:4]))
                del self.buf[:4]
                self.resyncs += 1
                continue
            need = 8 + w * h * 2
            if len(self.buf) < need:
                break
            px = bytes(self.buf[8:need])
            del self.buf[:need]
            self.frames += 1
            newest = (w, h, px)
            # Loop again: if several frames arrived, only the last is wanted.
        return newest


def rgb565_to_rgb(px: bytes, w: int, h: int):
    out = bytearray(w * h * 3)
    for i in range(w * h):
        v = px[i * 2] | (px[i * 2 + 1] << 8)
        out[i * 3]     = ((v >> 11) & 0x1F) * 255 // 31
        out[i * 3 + 1] = ((v >> 5) & 0x3F) * 255 // 63
        out[i * 3 + 2] = (v & 0x1F) * 255 // 31
    return bytes(out)


def save_ppm(path, w, h, px):
    with open(path, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (w, h))
        fh.write(rgb565_to_rgb(px, w, h))


# ---- terminal back end ---------------------------------------------------

class TermView:
    """
    Half-block rendering: each character cell is two vertical pixels, so a
    128x128 screen fits 128 columns by 64 rows. Needs no libraries and works
    over ssh, which is where this usually runs.
    """

    name = "terminal"

    def __init__(self):
        self.fd = sys.stdin.fileno()
        self.saved = termios.tcgetattr(self.fd)
        tty.setcbreak(self.fd)
        sys.stdout.write("\x1b[?25l")   # hide cursor

    def close(self):
        sys.stdout.write("\x1b[?25h\x1b[0m\n")
        termios.tcsetattr(self.fd, termios.TCSADRAIN, self.saved)

    def draw(self, w, h, px, status):
        rgb = rgb565_to_rgb(px, w, h)
        rows = []
        for y in range(0, h - 1, 2):
            line = []
            prev = None
            for x in range(w):
                t = (y * w + x) * 3
                b = ((y + 1) * w + x) * 3
                top = (rgb[t], rgb[t + 1], rgb[t + 2])
                bot = (rgb[b], rgb[b + 1], rgb[b + 2])
                if (top, bot) != prev:
                    line.append("\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm" %
                                (top + bot))
                    prev = (top, bot)
                line.append("\u2580")   # upper half block
            rows.append("".join(line) + "\x1b[0m")
        sys.stdout.write("\x1b[H" + "\n".join(rows) + "\x1b[0m\n" + status + "\x1b[K")
        sys.stdout.flush()

    def keys(self):
        """Non-blocking; returns a list of logical key names."""
        out = []
        while select.select([sys.stdin], [], [], 0)[0]:
            ch = sys.stdin.read(1)
            if ch == "\x1b":
                seq = ""
                while select.select([sys.stdin], [], [], 0.02)[0] and len(seq) < 4:
                    seq += sys.stdin.read(1)
                out.append({"[A": "up", "[B": "down",
                            "[5~": "pgup", "[6~": "pgdn"}.get(seq, "?"))
            elif ch in ("\r", "\n"):
                out.append("enter")
            elif ch in ("\x7f", "\b"):
                out.append("back")
            elif ch == "\t":
                out.append("tab")
            elif ch == "f":
                out.append("fav")
            elif ch == "p":
                out.append("shot")
            elif ch in ("q", "\x03"):
                out.append("quit")
        return out


# ---- Tk back end ---------------------------------------------------------

class TkView:
    name = "tk"

    def __init__(self, scale=4):
        import tkinter as tk
        self.tk = tk
        self.scale = scale
        self.root = tk.Tk()
        self.root.title("gbagate - emulated ST7735")
        self.root.configure(bg="#101020")
        self.canvas = tk.Canvas(self.root, width=DEFAULT_W * scale,
                                height=DEFAULT_H * scale, highlightthickness=0,
                                bg="#101020")
        self.canvas.pack()
        self.status = tk.Label(self.root, text="", fg="#c8d0e0", bg="#101020",
                               font=("monospace", 10), anchor="w")
        self.status.pack(fill="x")
        self.img = None
        self.pending = []
        self.alive = True

        for seq, name in (("<Up>", "up"), ("<Down>", "down"),
                          ("<Prior>", "pgup"), ("<Next>", "pgdn"),
                          ("<Return>", "enter"), ("<KP_Enter>", "enter"),
                          ("<BackSpace>", "back"), ("<Tab>", "tab"),
                          ("f", "fav"), ("p", "shot"), ("q", "quit")):
            self.root.bind(seq, lambda e, n=name: self.pending.append(n))
        self.root.protocol("WM_DELETE_WINDOW",
                           lambda: self.pending.append("quit"))

    def close(self):
        self.alive = False
        try:
            self.root.destroy()
        except Exception:
            pass

    def draw(self, w, h, px, status):
        rgb = rgb565_to_rgb(px, w, h)
        ppm = b"P6\n%d %d\n255\n" % (w, h) + rgb
        self.img = self.tk.PhotoImage(data=ppm).zoom(self.scale, self.scale)
        self.canvas.delete("all")
        self.canvas.create_image(0, 0, anchor="nw", image=self.img)
        self.status.config(text=status)
        self.root.update_idletasks()
        self.root.update()

    def keys(self):
        try:
            self.root.update()
        except Exception:
            return ["quit"]
        out, self.pending = self.pending, []
        return out


def pick_view(mode):
    if mode == "term":
        return TermView()
    try:
        return TkView()
    except Exception as exc:
        if mode == "tk":
            sys.exit(f"Tk unavailable: {exc}\nTry --term.")
        print(f"Tk unavailable ({exc}); using the terminal view", file=sys.stderr)
        time.sleep(1)
        return TermView()


# ---- main ---------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fb", default="127.0.0.1:5557",
                    help="where QEMU exposes UART2 (default 127.0.0.1:5557)")
    ap.add_argument("--term", action="store_const", dest="mode", const="term",
                    help="force the terminal view")
    ap.add_argument("--tk", action="store_const", dest="mode", const="tk",
                    help="force the Tk window")
    ap.add_argument("--shot-dir", default=".", help="where 'p' saves frames")
    ap.add_argument("--console-log", metavar="PATH",
                    help="write the non-frame bytes on this socket to PATH. "
                         "Needed with --aux-uart 0, where the ESP-IDF console "
                         "shares the framebuffer UART: those bytes are the log, "
                         "and this is the only place they can be kept.")
    ap.set_defaults(mode="auto")
    args = ap.parse_args()

    host, _, port = args.fb.rpartition(":")
    host = host or "127.0.0.1"

    print(f"connecting to {host}:{port} ...", file=sys.stderr)
    deadline = time.time() + 30
    sock = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection((host, int(port)), timeout=2)
            break
        except OSError:
            time.sleep(0.3)
    if sock is None:
        sys.exit(f"no framebuffer stream on {host}:{port} -- is ./qemu/run.sh going?")
    sock.setblocking(False)

    # Opened before the view so a bad path fails immediately with a plain
    # traceback, rather than after Tk has grabbed the terminal.
    junk = None
    if args.console_log:
        junk = open(args.console_log, "wb")
        print(f"console bytes on this socket -> {args.console_log}",
              file=sys.stderr)

    stream = FrameStream(sock, junk)
    view = pick_view(args.mode)
    shots = 0
    last = None
    started = time.time()

    try:
        while True:
            try:
                frame = stream.poll(0.03)
            except ConnectionError as exc:
                print(f"\n{exc}", file=sys.stderr)
                break
            if frame:
                last = frame

            for key in view.keys():
                if key == "quit":
                    return
                if key == "shot":
                    if last:
                        path = os.path.join(args.shot_dir, f"viewer{shots}.ppm")
                        save_ppm(path, *last)
                        shots += 1
                    continue
                b = KEYMAP.get(key)
                if b:
                    try:
                        sock.sendall(b)
                    except OSError:
                        print("\ninput channel closed", file=sys.stderr)
                        return

            if last:
                fps = stream.frames / max(time.time() - started, 0.001)
                # Skipped bytes are only worth showing once there are some. In
                # the shared-console layout they are the log and climb steadily;
                # on a dedicated UART a number that keeps rising means the
                # stream is being corrupted rather than merely shared.
                shared = (f"  console={stream.junk_bytes}B"
                          if stream.junk_bytes else "")
                view.draw(*last,
                          status=f"{view.name}  frames={stream.frames} "
                                 f"({fps:4.1f}/s)  resyncs={stream.resyncs}"
                                 f"{shared}  up/down enter  q=quit")
    except KeyboardInterrupt:
        pass
    finally:
        view.close()
        sock.close()
        if junk is not None:
            junk.close()


if __name__ == "__main__":
    main()
