"""accurate_boot.py -- boot a hardware image on qemu_esp32 with its board's
parts wired on, and check what the board's own drivers do with them.

    python3 -I qemu/accurate_boot.py <qemu> <flash.bin> <board> <variant> <out-dir>

Boards (the pins are port/esp32s3/board.h's):
  devkit         the ST7735 128x128 on GP-SPI2
  carrier-touch  the ILI9341 320x240 with its XPT2046, an SD card holding
                 rom/tester.gba, and the Battle Chip board's MCP23017s on I2C

Checked on every board: app_main runs, the link PHYs start on core 1 on
real GPIO, every task starts, nothing panics or resets; the panel comes up
and the picture is read back from the emulated glass (QMP screendump).

On carrier-touch, also: the card mounts; the screen is the 128x128 UI at
15/8 in the middle (every doubled column and row is a true copy), with the
ten touch keys either side, in the theme's colours (so the bytes go out in
the right order); a touch on the down key draws that key pressed and moves
the menu, and lifting the finger draws it released; touch alone reaches
Send Program, which lists the program on the card.

QEMU's own output goes to <out-dir>/qemu.log, the console to console.log,
screenshots to <out-dir>/*.ppm. QEMU_BIOS: the ROM directory, for a QEMU
that is not in its build tree (qemu_esp32's CI artifact).
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails = 0


def check(what, ok, detail=''):
    global fails
    print('  %s  %s%s' % ('ok  ' if ok else 'FAIL', what, (' (%s)' % detail) if detail and not ok else ''))
    fails += 0 if ok else 1
    return ok


# ---- colours: the theme's RGB565, as the panel model shows it --------------

def c565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def glass(v):
    r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


CLASSIC = {k: glass(c565(*v)) for k, v in {
    'bg': (16, 20, 32), 'row_alt': (28, 34, 50), 'text': (240, 244, 248),
    'accent': (255, 208, 64), 'header': (232, 120, 24)}.items()}

# src/touch_pad.h
PANEL_W, PANEL_H, STRIP_W, UI_X, UI_SIZE, KEY_H = 320, 240, 40, 40, 240, 48
RIGHT_X = UI_X + UI_SIZE
LEFT_KEYS = ['LEFT', 'L', 'UP', 'DOWN', 'R']
RIGHT_KEYS = ['RIGHT', 'A', 'B', 'SELECT', 'START']


def tp_src(p):
    return p * 8 // 15


def key_centre(name):
    if name in LEFT_KEYS:
        return STRIP_W // 2, LEFT_KEYS.index(name) * KEY_H + KEY_H // 2
    return RIGHT_X + STRIP_W // 2, RIGHT_KEYS.index(name) * KEY_H + KEY_H // 2


def face_at(name):
    """A point on a key's face clear of its label: just inside the top-left
    corner of the box (touch_pad.c leaves a 2px gap round each box)."""
    x, y = key_centre(name)
    return x - STRIP_W // 2 + 3, y - KEY_H // 2 + 3


def raw_for(x, y):
    """A panel point to the XPT2046's raw plates, by its default calibration
    (200..3900 on both, X along the 320 side), which the firmware's default
    (tp_cal_default) inverts."""
    return 200 + (3900 - 200) * x // (PANEL_W - 1), 200 + (3900 - 200) * y // (PANEL_H - 1)


# ---- boards ------------------------------------------------------------------

def board_args(board, out):
    if board == 'devkit':
        pins = header_pins('BOARD_DEVKIT')
        # A BGR module, as most are and as the driver assumes (Settings flips it).
        return ['-device', 'st7735,bus=spi2,id=lcd,width=128,height=128,x-offset=2,y-offset=3,panel-bgr=on',
                '-device', 'esp32s3.pin-link,pin=%d,dev=lcd,line=ssi-gpio-cs,pull=up' % pins['PIN_LCD_CS'],
                '-device', 'esp32s3.pin-link,pin=%d,dev=lcd,line=dc' % pins['PIN_LCD_DC']]
    if board == 'carrier-touch':
        pins = header_pins('BOARD_CARRIER_TOUCH')
        card = make_card(out)
        return ['-device', 'ili9341,bus=spi2,id=lcd',
                '-device', 'esp32s3.pin-link,pin=%d,dev=lcd,line=ssi-gpio-cs,pull=up' % pins['PIN_LCD_CS'],
                '-device', 'esp32s3.pin-link,pin=%d,dev=lcd,line=dc' % pins['PIN_LCD_DC'],
                '-device', 'xpt2046,bus=spi2,cs=1,id=touch',
                '-device', 'esp32s3.pin-link,pin=%d,dev=touch,line=ssi-gpio-cs,pull=up' % pins['PIN_TOUCH_CS'],
                '-device', 'esp32s3.pin-link,pin=%d,dev=touch,line=penirq,dir=out,open-drain=on,pull=up'
                % pins['PIN_TOUCH_IRQ'],
                '-drive', 'id=card,if=none,format=raw,file=%s' % card,
                '-device', 'ssi-sd,bus=spi2,cs=2,id=sd,drive=card',
                '-device', 'esp32s3.pin-link,pin=%d,dev=sd,line=ssi-gpio-cs,pull=up' % pins['PIN_SD_CS'],
                # The Battle Chip board: two expanders on I2C0, as doc/CARRIER.md wires it.
                '-device', 'mcp23017,bus=i2c0,address=0x20,id=exp0',
                '-device', 'mcp23017,bus=i2c0,address=0x21,id=exp1']
    raise SystemExit('unknown board %s' % board)


def header_pins(board_macro):
    """The pins board.h gives a board, through the C preprocessor rather than
    a copy here. A pin the board does not have is left out."""
    names = ['PIN_LCD_CS', 'PIN_LCD_DC', 'PIN_SD_CS', 'PIN_TOUCH_CS', 'PIN_TOUCH_IRQ']
    src = '#define %s 1\n#include "board.h"\n' % board_macro
    src += ''.join('#ifdef %s\npin "%s" %s\n#endif\n' % (n, n, n) for n in names)
    p = subprocess.run(['cc', '-E', '-P', '-I', os.path.join(ROOT, 'port/esp32s3'), '-x', 'c', '-'],
                       input=src, capture_output=True, text=True, check=True)
    out = {}
    for line in p.stdout.splitlines():
        f = line.split()
        if len(f) >= 3 and f[0] == 'pin':
            out[f[1].strip('"')] = int(''.join(f[2:]).strip('()'))
    return out


def make_card(out):
    """A 32 MB FAT card with rom/tester.gba on it (mtools: no root, no mount)."""
    img = os.path.join(out, 'sd.img')
    with open(img, 'wb') as f:
        f.truncate(32 << 20)
    subprocess.run(['mformat', '-i', img, '::'], check=True)       # FAT16 at this size
    subprocess.run(['mcopy', '-i', img, os.path.join(ROOT, 'rom/tester.gba'), '::TESTER.GBA'], check=True)
    return img


# ---- QMP ---------------------------------------------------------------------

class QMP:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.05)
        self.f = self.s.makefile('rw')
        self.f.readline()                            # the greeting
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r:
                return r['return']
            if 'error' in r:
                raise RuntimeError('%s: %s' % (name, r['error']))

    def shot(self, path):
        if os.path.exists(path):
            os.remove(path)
        self.cmd('screendump', filename=path, device='lcd')
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) > 0:
                break
            time.sleep(0.02)
        return landscape(read_ppm(path))

    def touch(self, x, y, pressed):
        rx, ry = raw_for(x, y)
        self.cmd('qom-set', path='/machine/peripheral/touch', property='x', value=rx)
        self.cmd('qom-set', path='/machine/peripheral/touch', property='y', value=ry)
        self.cmd('qom-set', path='/machine/peripheral/touch', property='pressed', value=pressed)


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts, i = [], 0
    while len(parts) < 4:                            # P6 w h max, then one space
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    px = data[i:i + w * h * 3]
    return w, h, [tuple(px[k:k + 3]) for k in range(0, len(px), 3)]


def at(img, x, y):
    w, _, px = img
    return px[y * w + x]


def landscape(img):
    """The ILI9341's glass is 240x320, portrait; the firmware draws across it
    in landscape (MADCTL MV), so the screenshot holds the picture turned a
    quarter clockwise. Turned back: landscape (x, y) is glass (239 - y, x)."""
    w, h, px = img
    if (w, h) != (PANEL_H, PANEL_W):
        return img
    out = [px[lx * w + (w - 1 - ly)] for ly in range(PANEL_H) for lx in range(PANEL_W)]
    return PANEL_W, PANEL_H, out


# ---- the run -----------------------------------------------------------------

def wait_for(log, text, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(log) and text in open(log, errors='replace').read():
            return True
        time.sleep(0.1)
    return False


def main(qemu, flash, board, variant, out):
    os.makedirs(out, exist_ok=True)
    console, qlog, sock = (os.path.join(out, n) for n in ('console.log', 'qemu.log', 'qmp.sock'))
    for p in (console, sock):
        if os.path.exists(p):
            os.remove(p)
    print('%s, %s image, on qemu_esp32' % (board, variant))
    args = [qemu, '-machine', 'esp32s3', '-m', '8M',
            '-drive', 'file=%s,if=mtd,format=raw' % flash,
            '-serial', 'file:%s' % console, '-display', 'none', '-no-reboot', '-monitor', 'none',
            '-qmp', 'unix:%s,server=on,wait=off' % sock] + board_args(board, out)
    if os.environ.get('QEMU_BIOS'):                    # a QEMU moved from its build tree
        args += ['-L', os.environ['QEMU_BIOS']]
    q = subprocess.Popen(args, stdout=open(qlog, 'w'), stderr=subprocess.STDOUT)
    try:
        return run(q, QMP(sock), console, board, variant, out)
    finally:
        q.kill()
        q.wait()
        if fails:
            print('  QEMU said:\n    ' + '\n    '.join(open(qlog, errors='replace').read().splitlines()[-5:]))


def run(q, qmp, console, board, variant, out):
    up = wait_for(console, 'tasks started for the %s image' % variant, 40)
    time.sleep(2)                                     # the first frame out
    log = open(console, errors='replace').read()
    check('app_main runs', 'Calling app_main()' in log)
    check('every task starts', up)
    check('the Multi-Play PHY starts on core 1', 'mp_phy: Multi-Play PHY on core 1' in log)
    check('the Wireless Adapter PHY starts on core 1', 'rfu_phy: Wireless Adapter PHY on core 1' in log)
    check('the link source is the PHY', 'link source: phy' in log)

    img = qmp.shot(os.path.join(out, 'boot.ppm'))
    w, h, _ = img
    if board == 'devkit':
        check('the ST7735 driver comes up', 'st7735: 128x128' in log)
        check('the glass is 128x128', (w, h) == (128, 128), '%dx%d' % (w, h))
        check('there is a picture', len(set(img[2])) > 3)
        # The title bar is the theme's header colour: the bytes reach the
        # glass in the right order, and red and blue are where they belong.
        check("the theme's colours reach the glass (the header bar)", CLASSIC['header'] in set(img[2]),
              'colours seen: %s' % sorted(set(img[2]), key=lambda c: -img[2].count(c))[:4])
    else:
        carrier_touch(qmp, log, img, console, out)

    time.sleep(0.5)
    log = open(console, errors='replace').read()
    for bad in ('Guru Meditation', 'abort()', 'panic', 'Task watchdog', 'Backtrace'):
        if bad in log:
            check('no panic, abort or watchdog', False, bad)
            break
    else:
        check('no panic, abort or watchdog', True)
    check('no reset after boot', log.count('rst:0x') <= 1)
    check('QEMU still running', q.poll() is None)
    print('%s on qemu_esp32: %s' % (board, 'all checks passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


def carrier_touch(qmp, log, img, console, out):
    w, h, px = img
    check('the ILI9341 driver comes up, landscape', 'lcd: ILI9341 320x240 landscape' in log)
    check('the touch controller is the input', 'touch: XPT2046 on CS=' in log)
    check('the SD card mounts', 'sd: card mounted' in log)
    check('the picture is 320x240 across (the 240x320 glass, drawn in landscape)',
          (w, h) == (PANEL_W, PANEL_H), '%dx%d' % (w, h))
    if (w, h) != (PANEL_W, PANEL_H):
        return

    # The square: the 128x128 UI at 15/8. Every panel column (row) showing the
    # same UI column (row) as its neighbour must be a true copy of it.
    copies = bad = 0
    for p in range(UI_SIZE - 1):
        if tp_src(p) == tp_src(p + 1):
            copies += 1
            bad += any(at(img, UI_X + p, y) != at(img, UI_X + p + 1, y) for y in range(0, PANEL_H, 3))
            bad += any(at(img, UI_X + x, p) != at(img, UI_X + x, p + 1) for x in range(0, UI_SIZE, 3))
    check('the UI is scaled 15/8 into the square (%d doubled lines, all copies)' % copies,
          copies == 112 and bad == 0, '%d differ' % bad)
    check('the UI square has a picture', len({at(img, UI_X + x, y) for x in range(0, UI_SIZE, 4)
                                              for y in range(0, PANEL_H, 4)}) >= 3)

    # The keys, in the theme's colours: the 2px gap is the background, a key
    # face the band colour, and the labels the text colour. Wrong byte order
    # or red and blue swapped would fail all three.
    check('the gap between keys is the theme background', at(img, 0, 0) == CLASSIC['bg'],
          '%s vs %s' % (at(img, 0, 0), CLASSIC['bg']))
    for name in LEFT_KEYS + RIGHT_KEYS:
        x, y = key_centre(name)
        face = at(img, *face_at(name))
        ink = {at(img, xx, yy) for xx in range(x - 16, x + 16) for yy in range(y - 18, y + 18)}
        if face != CLASSIC['row_alt'] or CLASSIC['text'] not in ink:
            check('key %s drawn (face, label)' % name, False, '%s %s' % (face, CLASSIC['text'] in ink))
            break
    else:
        check('all ten keys drawn: faces in the band colour, labels in the text colour', True)

    # A touch on DOWN: drawn pressed, the menu moves; lifted, drawn released.
    x, y = key_centre('DOWN')
    qmp.touch(x, y, True)
    time.sleep(0.25)
    held = qmp.shot(os.path.join(out, 'down-held.ppm'))
    qmp.touch(x, y, False)
    time.sleep(0.4)
    after = qmp.shot(os.path.join(out, 'down-after.ppm'))
    check('a touch on DOWN draws it pressed (the accent colour)',
          at(held, *face_at('DOWN')) == CLASSIC['accent'], str(at(held, *face_at('DOWN'))))
    check('lifting the finger draws it released', at(after, *face_at('DOWN')) == CLASSIC['row_alt'])
    moved = sum(at(img, UI_X + xx, yy) != at(after, UI_X + xx, yy)
                for xx in range(0, UI_SIZE, 2) for yy in range(0, PANEL_H, 2))
    check('and the menu moved (%d pixels of the square changed)' % moved, moved > 200)
    check('the other strip did not change',
          all(at(img, xx, yy) == at(after, xx, yy) for xx in range(RIGHT_X, PANEL_W) for yy in range(PANEL_H)))

    # By touch alone to Send Program: up to the top, down one, OK. The gate
    # image's menu opens with Link (USB, Wireless and Online in one entry),
    # then Send Program.
    for _ in range(12):
        tap(qmp, 'UP')
    tap(qmp, 'DOWN')
    before = len(open(console, errors='replace').read())
    tap(qmp, 'A')
    time.sleep(1.5)
    new = open(console, errors='replace').read()[before:]
    qmp.shot(os.path.join(out, 'send-program.ppm'))
    check('touch alone reaches Send Program, which reads the card',
          'sd: 1 programs' in new, new.strip().splitlines()[-1] if new.strip() else 'no log')


def tap(qmp, name, hold=0.12):
    x, y = key_centre(name)
    qmp.touch(x, y, True)
    time.sleep(hold)
    qmp.touch(x, y, False)
    time.sleep(0.15)


if __name__ == '__main__':
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    sys.exit(main(*sys.argv[1:]))
