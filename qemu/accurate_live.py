#!/usr/bin/env python3
"""Interactive carrier-touch front-end for the accurate ESP32-S3 QEMU."""

from pathlib import Path
import os
import hashlib
import json
import shutil
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "qemu"))
import accurate_boot as harness

QEMU = ROOT / "build/esp-qemu/build/qemu-system-xtensa"
SOURCE_FLASH = Path(os.environ.get(
    "GBAGATE_SOURCE_FLASH",
    str(ROOT / "build/hw-gate-carrier-touch-idf556-ci-exact/flash-qemu.bin")))
LIVE = Path(os.environ.get("GBAGATE_LIVE_DIR", str(ROOT / "build/live-carrier-touch")))
FLASH = LIVE / "flash.bin"
SD = LIVE / "sd.img"
OUT = Path(os.environ.get("GBAGATE_LIVE_OUT", "/tmp/gbagate-live"))
SOCK = OUT / "qmp.sock"
FRAME = OUT / "frame.ppm"
CONSOLE = OUT / "console.log"
QLOG = OUT / "qemu.log"
MGBA_BRIDGE = os.environ.get("GBAGATE_MGBA_BRIDGE", "0") == "1"
LINK_PORT = os.environ.get("GBAGATE_LINK_PORT", "5556")


class QMP:
    def __init__(self, path, process):
        self.sock = socket.socket(socket.AF_UNIX)
        for _ in range(200):
            if process.poll() is not None:
                raise RuntimeError("QEMU exited; inspect " + str(QLOG))
            try:
                self.sock.connect(str(path))
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("Timed out waiting for QMP socket")

        self.file = self.sock.makefile("rwb", buffering=0)
        greeting = self.file.readline()
        if not greeting:
            raise RuntimeError("QEMU closed its QMP connection")
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        request = {"execute": name, "arguments": args}
        self.file.write(json.dumps(request).encode() + b"\n")
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            response = json.loads(line)
            if "return" in response:
                return response["return"]
            if "error" in response:
                raise RuntimeError(f"{name}: {response['error']}")

    def touch(self, x, y, pressed):
        rx, ry = harness.raw_for(x, y)
        self.cmd("qom-set", path="/machine/peripheral/touch",
                 property="x", value=rx)
        self.cmd("qom-set", path="/machine/peripheral/touch",
                 property="y", value=ry)
        self.cmd("qom-set", path="/machine/peripheral/touch",
                 property="pressed", value=pressed)

    def screenshot(self):
        FRAME.unlink(missing_ok=True)
        self.cmd("screendump", filename=str(FRAME), device="lcd")
        return harness.landscape(harness.read_ppm(str(FRAME)))

    def close(self):
        try:
            self.file.close()
            self.sock.close()
        except Exception:
            pass


def prepare():
    for path in (QEMU, SOURCE_FLASH, ROOT / "rom/tester.gba"):
        if not path.is_file():
            raise RuntimeError(f"Required file missing: {path}")

    LIVE.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)

    if not FLASH.exists():
        shutil.copy2(SOURCE_FLASH, FLASH)
        print("Created persistent flash copy:", FLASH)

    if not SD.exists():
        with SD.open("wb") as f:
            f.truncate(32 << 20)
        subprocess.run(["mformat", "-i", str(SD), "::"], check=True)
        subprocess.run([
            "mcopy", "-i", str(SD),
            str(ROOT / "rom/tester.gba"), "::TESTER.GBA"
        ], check=True)
        print("Created persistent SD card with TESTER.GBA:", SD)

    # Do not steal a socket from another active QEMU instance.
    if SOCK.exists():
        probe = socket.socket(socket.AF_UNIX)
        active = False
        try:
            probe.connect(str(SOCK))
            active = True
        except OSError:
            pass
        finally:
            probe.close()
        if active:
            raise RuntimeError("An existing QEMU session owns " + str(SOCK))
        SOCK.unlink()


def device_args():
    pins = harness.header_pins("BOARD_CARRIER_TOUCH")
    return [
        "-device", "ili9341,bus=spi2,id=lcd",
        "-device", f"esp32s3.pin-link,pin={pins['PIN_LCD_CS']},dev=lcd,line=ssi-gpio-cs,pull=up",
        "-device", f"esp32s3.pin-link,pin={pins['PIN_LCD_DC']},dev=lcd,line=dc",

        "-device", "xpt2046,bus=spi2,cs=1,id=touch",
        "-device", f"esp32s3.pin-link,pin={pins['PIN_TOUCH_CS']},dev=touch,line=ssi-gpio-cs,pull=up",
        "-device", f"esp32s3.pin-link,pin={pins['PIN_TOUCH_IRQ']},dev=touch,line=penirq,dir=out,open-drain=on,pull=up",

        "-drive", f"id=card,if=none,format=raw,file={SD}",
        "-device", "ssi-sd,bus=spi2,cs=2,id=sd,drive=card",
        "-device", f"esp32s3.pin-link,pin={pins['PIN_SD_CS']},dev=sd,line=ssi-gpio-cs,pull=up",

        "-device", "mcp23017,bus=i2c0,address=0x20,id=exp0",
        "-device", "mcp23017,bus=i2c0,address=0x21,id=exp1",
    ]


class InteractiveWindow:
    SCALE = 3

    # Keyboard equivalents of the ten physical touch keys.
    KEY_POSITIONS = {
        "Left": (20, 24),
        "l": (20, 72),
        "Up": (20, 120),
        "Down": (20, 168),
        "r": (20, 216),
        "Right": (300, 24),
        "Return": (300, 72),
        "BackSpace": (300, 120),
        "f": (300, 168),
        "m": (300, 216),
    }

    def __init__(self, qmp, process):
        import tkinter as tk

        self.tk = tk
        self.qmp = qmp
        self.process = process
        self.root = tk.Tk()
        self.root.title("GBAGate — carrier-touch — accurate ESP32-S3 QEMU")
        self.root.resizable(False, False)

        self.canvas = tk.Canvas(
            self.root, width=320 * self.SCALE,
            height=240 * self.SCALE, highlightthickness=0
        )
        self.canvas.pack()
        self.status = tk.Label(
            self.root,
            text="Click the side keys. Arrows = directions, Enter = A, "
                 "Backspace = B, f = Select, m = Start. Close window to stop.",
            anchor="w", padx=8
        )
        self.status.pack(fill="x")

        self.image = None
        self.image_item = self.canvas.create_image(0, 0, anchor="nw")
        self.mouse_down = False
        self.key_down = set()
        self.key_release_timers = {}
        self.auto_touch_position = None
        self.closed = False

        self.canvas.bind("<ButtonPress-1>", self.on_mouse_down)
        self.canvas.bind("<B1-Motion>", self.on_mouse_move)
        self.canvas.bind("<ButtonRelease-1>", self.on_mouse_up)

        self.root.bind("<KeyPress>", self.on_key_down)
        self.root.bind("<KeyRelease>", self.on_key_up)
        self.root.protocol("WM_DELETE_WINDOW", self.shutdown)
        self.canvas.focus_set()

        self.root.after(500, self.refresh)
        if os.getenv("GBAGATE_AUTO_SELFTEST", "0") == "1":
            # Wait for firmware boot and the initial mode menu.
            self.root.after(3500, self.start_auto_selftest)

    def send_touch(self, x, y, down):
        x = max(0, min(319, x))
        y = max(0, min(239, y))
        self.qmp.touch(x, y, down)

    def mouse_point(self, event):
        return event.x // self.SCALE, event.y // self.SCALE

    def on_mouse_down(self, event):
        self.mouse_down = True
        self.send_touch(*self.mouse_point(event), True)

    def on_mouse_move(self, event):
        if self.mouse_down:
            self.send_touch(*self.mouse_point(event), True)

    def on_mouse_up(self, event):
        if self.mouse_down:
            self.send_touch(*self.mouse_point(event), False)
            self.mouse_down = False

    def release_key(self, key):
        timer = self.key_release_timers.pop(key, None)
        if timer is not None:
            try:
                self.root.after_cancel(timer)
            except Exception:
                pass
        if key in self.key_down:
            try:
                self.send_touch(*self.KEY_POSITIONS[key], False)
            finally:
                self.key_down.discard(key)

    def on_key_down(self, event):
        key = event.keysym
        if key == "Escape":
            self.shutdown()
            return
        if key in self.KEY_POSITIONS and key not in self.key_down:
            self.key_down.add(key)
            self.send_touch(*self.KEY_POSITIONS[key], True)
            self.key_release_timers[key] = self.root.after(
                250, lambda k=key: self.release_key(k)
            )

    def on_key_up(self, event):
        self.release_key(event.keysym)

    def start_auto_selftest(self):
        """Select Self Test, then BN6 US, using explicit QMP touch taps."""
        if self.closed:
            return
        self.auto_selftest_keys = (
            "Down", "Return",       # USB Storage -> Self Test
            "Down", "Down", "Down", # BN4 -> BN6 US
            "Return",
        )
        self.auto_selftest_index = 0
        self.status.config(text="Automated Self Test: selecting BN6 US")
        self.auto_selftest_tap_next()

    def auto_selftest_tap_next(self):
        if self.closed:
            return

        if self.auto_selftest_index >= len(self.auto_selftest_keys):
            self.status.config(
                text="Automated selection sent: Self Test / BN6 US"
            )
            return

        key = self.auto_selftest_keys[self.auto_selftest_index]
        x, y = self.KEY_POSITIONS[key]
        self.auto_touch_position = (x, y)

        try:
            self.send_touch(x, y, True)
        except Exception as exc:
            try:
                self.send_touch(x, y, False)
            except Exception:
                pass
            self.auto_touch_position = None
            self.status.config(text=f"Automated input failed: {exc}")
            return

        # One bounded touch pulse; the release is sent through the same QMP
        # connection that asserted the touch.
        self.root.after(120, self.auto_selftest_release)

    def auto_selftest_release(self):
        if self.closed:
            return

        pos = self.auto_touch_position
        if pos is not None:
            try:
                self.send_touch(pos[0], pos[1], False)
            except Exception as exc:
                self.auto_touch_position = None
                self.status.config(text=f"Touch release failed: {exc}")
                return
            self.auto_touch_position = None

        self.auto_selftest_index += 1
        self.root.after(450, self.auto_selftest_tap_next)

    def refresh(self):
        if self.closed:
            return
        if self.process.poll() is not None:
            self.status.config(text="QEMU stopped. See " + str(QLOG))
            self.root.after(1500, self.shutdown)
            return
        try:
            w, h, pixels = self.qmp.screenshot()
            rgb = bytes(c for pixel in pixels for c in pixel)
            ppm = f"P6\n{w} {h}\n255\n".encode() + rgb
            self.image = self.tk.PhotoImage(data=ppm).zoom(
                self.SCALE, self.SCALE
            )
            self.canvas.itemconfigure(self.image_item, image=self.image)
            self.status.config(
                text=f"Live LCD: {w}x{h} landscape | "
                     "Click side keys or use keyboard | Esc closes QEMU"
            )
        except Exception as exc:
            self.status.config(text=f"QEMU display/QMP error: {exc}")
        self.root.after(125, self.refresh)

    def shutdown(self):
        if self.closed:
            return
        self.closed = True
        if self.auto_touch_position is not None:
            try:
                self.send_touch(*self.auto_touch_position, False)
            except Exception:
                pass
            self.auto_touch_position = None
        for key in tuple(self.key_down):
            self.release_key(key)
        try:
            self.qmp.touch(160, 120, False)
        except Exception:
            pass
        try:
            self.process.terminate()
            self.process.wait(timeout=5)
        except Exception:
            try:
                self.process.kill()
                self.process.wait(timeout=3)
            except Exception:
                pass
        self.qmp.close()
        self.root.destroy()


def main():
    prepare()
    print("Flash SHA-256:", hashlib.sha256(FLASH.read_bytes()).hexdigest())
    print("Persistent emulated flash:", FLASH)
    print("Persistent emulated SD card:", SD)

    args = [
        str(QEMU), "-machine", "esp32s3", "-m", "8M",
        "-drive", f"file={FLASH},if=mtd,format=raw",
        "-serial", f"file:{CONSOLE}",
    ]
    if MGBA_BRIDGE:
        args += ["-serial", f"tcp::{LINK_PORT},server,nowait"]
        print(f"mGBA link UART1: tcp://127.0.0.1:{LINK_PORT}")
    args += [
        "-display", "none", "-no-reboot", "-monitor", "none",
        "-qmp", f"unix:{SOCK},server=on,wait=off",
    ] + device_args()

    with QLOG.open("wb") as qlog:
        process = subprocess.Popen(
            args, cwd=ROOT, stdout=qlog, stderr=subprocess.STDOUT
        )
        qmp = None
        try:
            qmp = QMP(SOCK, process)
            print("QEMU is running. Opening interactive LCD window.")
            window = InteractiveWindow(qmp, process)
            window.root.mainloop()
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            if qmp:
                qmp.close()
            SOCK.unlink(missing_ok=True)
            print("QEMU stopped.")
            print("Serial log:", CONSOLE)
            print("QEMU log:", QLOG)


if __name__ == "__main__":
    main()
