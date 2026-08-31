#!/usr/bin/env python3
"""
test_rom_selftest.py -- prove the tester ROM's logic and the display path with
no emulator, no ESP-IDF and no GBA hardware.

Four independent checks, all of which run with just a C compiler and python3:

  1. The ROM's handshake driver (rom/handshake.h) is compiled against the real
     src/chipgate.c and driven through a full round trip. It must take every one
     of the four gate identities to a linked, rotating session -- and, as a
     control, a driver that never sends the sync word must fail to link, or the
     check proves nothing. This is the same "prove the driver before the
     emulator" idea as test_gate_drive.py, but for the ROM's own logic.

  2. Chip injection, which nothing else in the tree exercises. broker.py,
     qemu/gate_sim.c and the tester ROM all leave the slot empty, so slot 5 is
     0000 in every capture taken so far -- the chip path, which is the entire
     point of a Battle Chip Gate, has never been on the wire. Here a chip is
     slotted on a live session and every word of the rotation is checked against
     an independent model: slot ordering, both counter step rules, the chip
     holding slot 5 for CG_CHIP_REPEATS frames, the slot emptying afterwards and
     chip_consumed firing exactly once.

     The model matters because hs_passed() is loose on purpose -- it only wants
     the identity, the filler and one varied slot, so it would happily pass a
     gate whose slots came round in the wrong order or whose counters stepped
     wrongly. The ROM cannot tell; this can.

  3. Session lifecycle: the game's closing acknowledge tears the session down,
     the gate goes back to holding its identity out, and it can link again. A
     disabled gate stays transparent and never links.

  4. The framebuffer path (qemu/fbgrab.py) is fed a synthetic FBUF frame and its
     PPM output is compared byte-for-byte against the RGB565->RGB888 conversion
     that test/test_render.c uses. A capture from live firmware under QEMU and a
     capture from the host render test are then guaranteed to be comparable.

If this passes, a failure under QEMU is the emulator's or the firmware's, not
the ROM logic's or the grabber's.
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbgrab  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The ROM's handshake.h is driven against the real chipgate.c here. The loop
# models the one-frame observation lag of the real ROM: the word staged from the
# previous answer is what the gate sees, and the gate's reply is the next answer.
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chipgate.h"
#include "handshake.h"

/*
 * Drive the ROM's handshake against the gate until the session is up, and hand
 * back the gate's latest answer so the caller can carry the exchange on from
 * exactly where the link came up.
 */
static uint16_t link_up(chipgate_t *cg, hs_t *h) {
    uint16_t answer = 0x0000;
    for (int i = 0; i < 500 && cg->state != CG_LINK_ACTIVE; i++) {
        uint16_t word = hs_step(h, answer);
        answer = chipgate_step(cg, word);
    }
    return answer;
}

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "drive";
    cg_game_t game = (argc > 2) ? (cg_game_t)atoi(argv[2]) : CG_GAME_BN6_US;

    chipgate_t cg;
    chipgate_init(&cg, game);
    hs_t h;
    hs_init(&h, cg.gate_id);

    /*
     * Chip injection, observed from the game's side. This is the path no other
     * harness reaches: broker.py, gate_sim.c and the tester ROM all leave the
     * slot empty, so slot 5 is 0000 in every capture taken so far.
     */
    if (strcmp(mode, "chip") == 0) {
        uint16_t chip = (argc > 3) ? (uint16_t)strtoul(argv[3], NULL, 0) : 0x00A5;
        uint16_t answer = link_up(&cg, &h);

        /* One chip at a time: the second load must be refused while the first
         * is still going out, and CG_CHIP_NONE is never a loadable chip. */
        printf("load_ok=%d\n",   chipgate_load(&cg, chip) ? 1 : 0);
        printf("load_busy=%d\n", chipgate_load(&cg, chip) ? 1 : 0);
        printf("load_zero=%d\n", chipgate_load(&cg, CG_CHIP_NONE) ? 1 : 0);

        /* Eleven rotations: nine carry the chip, the rest prove it empties. */
        for (int i = 0; i < 99; i++) {
            unsigned slot = cg.slot;
            uint16_t word = hs_step(&h, answer);
            answer = chipgate_step(&cg, word);
            printf("step %u %04X %d\n", slot, answer,
                   chipgate_take_consumed(&cg) ? 1 : 0);
        }
        printf("hs_passed=%d\n", hs_passed(&h) ? 1 : 0);
        printf("final_chip=%04X\n", cg.chip_id);
        printf("cg_state=%d\n", (int)cg.state);
        return 0;
    }

    /* The game closes a session by re-sending its acknowledge; the gate has to
     * drop back to holding its identity out and be able to link again. */
    if (strcmp(mode, "teardown") == 0) {
        (void)link_up(&cg, &h);
        printf("sessions=%u\n", (unsigned)cg.sessions);
        printf("state_linked=%d\n", (int)cg.state);

        uint16_t after = chipgate_step(&cg, cg.ack_word);
        printf("word_after_ack=%04X\n", after);
        printf("state_after_ack=%d\n", (int)cg.state);
        printf("slot_after_ack=%d\n", (int)cg.slot);

        hs_t h2;
        hs_init(&h2, cg.gate_id);
        (void)link_up(&cg, &h2);
        printf("state_relinked=%d\n", (int)cg.state);
        printf("sessions_after=%u\n", (unsigned)cg.sessions);
        return 0;
    }

    /* A disabled gate goes transparent: filler only, and never links. */
    if (strcmp(mode, "disabled") == 0) {
        cg.enabled = false;
        (void)link_up(&cg, &h);
        printf("cg_state=%d\n", (int)cg.state);
        printf("hs_passed=%d\n", hs_passed(&h) ? 1 : 0);
        return 0;
    }

    uint16_t answer = 0x0000;   /* SIOMULTI0 before anything is exchanged */
    for (int i = 0; i < 1200; i++) {
        uint16_t word = (strcmp(mode, "nosync") == 0)
                        ? 0x0000               /* control: never drive it */
                        : hs_step(&h, answer);
        answer = chipgate_step(&cg, word);
    }

    printf("gate_id=%04X\n", cg.gate_id);
    printf("gate_id_seen=%04X\n", h.gate_id_seen);
    printf("cg_state=%d\n", (int)cg.state);
    printf("hs_phase=%d\n", (int)h.phase);
    printf("linked_frames=%u\n", (unsigned)h.linked_frames);
    printf("hs_passed=%d\n", hs_passed(&h) ? 1 : 0);
    printf("rotation=");
    for (int i = 0; i < 9; i++) printf("%04X ", h.rotation[i]);
    printf("\n");
    return 0;
}
'''

# gate key -> (expected gate id, chipgate.c game index)
GATES = {"bn4": (0xFFC6, 0), "bn5": (0xFFC7, 1),
         "bn6jp": (0xFFC4, 2), "bn6": (0xFF00, 3)}


def build(tmp):
    src, exe = os.path.join(tmp, "h.c"), os.path.join(tmp, "h")
    with open(src, "w") as f:
        f.write(HARNESS)
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra",
         "-I", os.path.join(ROOT, "src"),
         "-I", os.path.join(ROOT, "rom"),
         "-o", exe, src, os.path.join(ROOT, "src", "chipgate.c")],
        check=True)
    return exe


def run(exe, mode, game_idx):
    out = subprocess.run([exe, mode, str(game_idx)],
                         stdout=subprocess.PIPE, text=True, check=True).stdout
    kv = {}
    for line in out.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            kv[k.strip()] = v.strip()
    return kv


# ---- the 9-slot rotation, modelled independently of chipgate.c -----------
#
# hs_passed() in rom/handshake.h is deliberately loose: it only wants to have
# seen the identity, the 0xFFFF filler and one varied slot. It would pass a gate
# whose slots came round in the wrong order, whose counters stepped by the wrong
# amount, or that never sent a chip at all. This model is the strict counterpart
# -- every word is predicted from the slot index alone and compared exactly.

CG_CHIP_REPEATS = 9
DATA0_SEED, DATA1_SEED = 0x00FE, 0xFFFE
SLOT_NAMES = ["COMMAND", "UNK_0", "UNK_1", "DATA_0", "DATA_1",
              "ID", "UNK_2", "UNK_3", "END"]


def model_word(slot, st, gate_id):
    """The word chipgate.c owes for `slot`. Mutates `st`; returns (word, consumed)."""
    if slot == 0:                       # CG_SLOT_COMMAND
        return gate_id, False
    if slot in (1, 2):                  # CG_SLOT_UNK_0 / UNK_1
        return 0xFFFF, False
    if slot == 3:                       # CG_SLOT_DATA_0: 8-bit, +3
        v = st["data0"]
        st["data0"] = (v + 3) & 0x00FF
        return v, False
    if slot == 4:                       # CG_SLOT_DATA_1: top 6 bits, -3
        v = st["data1"]
        st["data1"] = (v - 3) & 0xFC00
        return v, False
    if slot == 5:                       # CG_SLOT_ID: the slotted chip
        if st["chip"] == 0:
            return 0x0000, False
        chip = st["chip"]
        st["reps"] += 1
        consumed = st["reps"] >= CG_CHIP_REPEATS
        if consumed:
            st["reps"], st["chip"] = 0, 0
        return chip, consumed
    return 0x0000, False                # UNK_2, UNK_3, END


def run_trace(exe, mode, game_idx, *extra):
    """Run a harness mode that emits `step <slot> <word> <consumed>` lines."""
    out = subprocess.run([exe, mode, str(game_idx), *extra],
                         stdout=subprocess.PIPE, text=True, check=True).stdout
    kv, steps = {}, []
    for line in out.splitlines():
        if line.startswith("step "):
            _, slot, word, consumed = line.split()
            steps.append((int(slot), int(word, 16), consumed == "1"))
        elif "=" in line:
            k, _, v = line.partition("=")
            kv[k.strip()] = v.strip()
    return kv, steps


def test_chip_injection() -> int:
    """The chip path: nothing else in the tree ever puts a chip on the wire."""
    print("\nchip injection through the full rotation")
    fails = 0
    chip = 0x00A5
    gate_id = 0xFF00           # BN6 US, what the QEMU firmware announces

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        kv, steps = run_trace(exe, "chip", 3, hex(chip))

        # The gate holds exactly one chip, and 0x0000 is not a chip.
        for key, want in (("load_ok", "1"), ("load_busy", "0"),
                          ("load_zero", "0")):
            if kv.get(key) != want:
                print(f"  FAIL {key}={kv.get(key)}, expected {want}")
                fails += 1
        if not fails:
            print("  one chip at a time: reload refused while busy, "
                  "CG_CHIP_NONE refused")

        if not steps:
            print("  FAIL harness produced no rotation trace")
            return fails + 1

        # 1. The slots must come round in order, wrapping 8 -> 0, starting at
        #    UNK_1 (the handshake burns COMMAND and UNK_0 -- see chipgate.c).
        if steps[0][0] != 2:
            print(f"  FAIL rotation resumed at slot {steps[0][0]} "
                  f"({SLOT_NAMES[steps[0][0]]}), expected UNK_1")
            fails += 1
        order_bad = [i for i in range(1, len(steps))
                     if steps[i][0] != (steps[i - 1][0] + 1) % 9]
        if order_bad:
            i = order_bad[0]
            print(f"  FAIL slot order broke at step {i}: "
                  f"{steps[i - 1][0]} -> {steps[i][0]}")
            fails += 1
        else:
            print(f"  {len(steps)} frames: slots cycle 0..8 in order, "
                  f"resuming at UNK_1")

        # 2. Every word must match the model exactly.
        st = {"data0": DATA0_SEED, "data1": DATA1_SEED, "chip": chip, "reps": 0}
        mismatch = None
        consumed_at = []
        for i, (slot, word, consumed) in enumerate(steps):
            want, want_consumed = model_word(slot, st, gate_id)
            if word != want and mismatch is None:
                mismatch = (i, slot, word, want)
            if consumed:
                consumed_at.append(i)
            if consumed != want_consumed and mismatch is None:
                mismatch = (i, slot, word, want)
        if mismatch:
            i, slot, got, want = mismatch
            print(f"  FAIL step {i} slot {slot} ({SLOT_NAMES[slot]}): "
                  f"got {got:04X}, model says {want:04X}")
            fails += 1
        else:
            print("  every word matches the documented rotation "
                  "(identity, filler, both counters, chip, zeros)")

        # 3. The chip occupies slot 5 for exactly CG_CHIP_REPEATS frames.
        id_words = [w for slot, w, _ in steps if slot == 5]
        carried = [w for w in id_words if w == chip]
        if len(carried) != CG_CHIP_REPEATS:
            print(f"  FAIL chip {chip:04X} appeared {len(carried)}x in slot 5, "
                  f"expected {CG_CHIP_REPEATS}")
            fails += 1
        elif id_words[:CG_CHIP_REPEATS] != [chip] * CG_CHIP_REPEATS:
            print(f"  FAIL chip was not carried on consecutive rotations: "
                  f"{[f'{w:04X}' for w in id_words]}")
            fails += 1
        else:
            print(f"  chip {chip:04X} held slot 5 for {CG_CHIP_REPEATS} "
                  f"consecutive rotations, then the slot emptied")

        # 4. The consumed edge fires once, on the last rotation carrying it.
        if len(consumed_at) != 1:
            print(f"  FAIL chip_consumed fired {len(consumed_at)}x, expected once")
            fails += 1
        else:
            if steps[consumed_at[0]][1] != chip:
                print("  FAIL chip_consumed fired on a frame not carrying the chip")
                fails += 1
            else:
                print("  chip_consumed is a single edge, on the last chip frame")

        if kv.get("final_chip") != "0000":
            print(f"  FAIL slot did not empty: final_chip={kv.get('final_chip')}")
            fails += 1
        if kv.get("cg_state") != "2":
            print(f"  FAIL gate left ACTIVE during chip transmission: "
                  f"state={kv.get('cg_state')}")
            fails += 1

        # 5. A chip on the wire must not disturb the ROM's own verdict.
        if kv.get("hs_passed") != "1":
            print("  FAIL the ROM's handshake stopped passing with a chip slotted")
            fails += 1
        else:
            print("  the ROM still reaches PASS with a chip on the wire")

    return fails


def test_session_lifecycle() -> int:
    """Teardown, relink, and the disabled (transparent) gate."""
    print("\nsession teardown, relink, and the disabled gate")
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)

        kv, _ = run_trace(exe, "teardown", 3)
        checks = (
            ("state_linked",   "2", "gate reached ACTIVE"),
            ("sessions",       "1", "one session counted"),
            ("word_after_ack", "FF00", "identity back on the wire after teardown"),
            ("state_after_ack", "0", "dropped to IDLE on the closing acknowledge"),
            ("slot_after_ack",  "0", "rotation rewound to COMMAND"),
            ("state_relinked",  "2", "linked again after teardown"),
            ("sessions_after",  "2", "second session counted"),
        )
        for key, want, why in checks:
            if kv.get(key) != want:
                print(f"  FAIL {key}={kv.get(key)}, expected {want} ({why})")
                fails += 1
        if not fails:
            print("  closing acknowledge tears the session down, "
                  "and the gate relinks cleanly")

        kv, _ = run_trace(exe, "disabled", 3)
        if kv.get("cg_state") != "0" or kv.get("hs_passed") != "0":
            print(f"  FAIL disabled gate: state={kv.get('cg_state')} "
                  f"hs_passed={kv.get('hs_passed')}, expected 0 and 0")
            fails += 1
        else:
            print("  a disabled gate stays transparent and never links")

    return fails


def test_handshake() -> int:
    print("handshake driver against the real chipgate.c")
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)

        for key, (gate_id, idx) in GATES.items():
            kv = run(exe, "drive", idx)
            state = int(kv["cg_state"])
            passed = kv["hs_passed"] == "1"
            seen = int(kv["gate_id_seen"], 16)
            # cg state 2 is CG_LINK_ACTIVE
            if state != 2 or not passed or seen != gate_id:
                print(f"  FAIL {key}: state={state} passed={passed} "
                      f"seen={seen:04X} want {gate_id:04X}")
                print(f"        rotation {kv.get('rotation')}")
                fails += 1
            else:
                print(f"  {key:5s} linked, gate {gate_id:04X}, "
                      f"{kv['linked_frames']} linked frames, "
                      f"rotation {kv['rotation']}")

        # Control: no sync word must never link, or the test proves nothing.
        kv = run(exe, "nosync", 3)
        if int(kv["cg_state"]) == 2:
            print("  FAIL a driver that never sends the sync word still linked")
            fails += 1
        else:
            print("  control: idle traffic alone does not link (as it must not)")

    return fails


def test_framebuffer() -> int:
    print("\nframebuffer path (fbgrab.py) vs test_render.c's conversion")
    fails = 0

    # A small frame with the awkward corners: black, white, pure R/G/B, mid.
    w, h = 4, 2
    px16 = [0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x8410, 0x1234, 0xABCD]
    pixels = bytearray()
    for c in px16:
        pixels += bytes([c & 0xFF, (c >> 8) & 0xFF])   # little-endian

    # The exact conversion test_render.c's save_ppm() performs.
    def expect_rgb(c):
        return bytes([
            ((c >> 11) & 0x1F) * 255 // 31,
            ((c >> 5) & 0x3F) * 255 // 63,
            (c & 0x1F) * 255 // 31,
        ])
    expected = f"P6\n{w} {h}\n255\n".encode("ascii") + b"".join(
        expect_rgb(c) for c in px16)

    frame = b"FBUF" + bytes([w & 0xFF, w >> 8, h & 0xFF, h >> 8]) + bytes(pixels)

    # Whole frame in one go.
    parser = fbgrab.FrameParser()
    got = list(parser.feed(frame))
    if len(got) != 1:
        print(f"  FAIL parsed {len(got)} frames from one, expected 1")
        return fails + 1
    gw, gh, gpix = got[0]
    ppm = fbgrab.ppm_bytes(gpix, gw, gh)
    if (gw, gh) != (w, h) or ppm != expected:
        print("  FAIL PPM does not match test_render.c's conversion")
        fails += 1
    else:
        print(f"  {w}x{h} frame -> PPM matches save_ppm() byte-for-byte")

    # Resync: junk, then a byte-at-a-time delivery straddling the magic.
    parser = fbgrab.FrameParser()
    stream = b"\x00\x11garbage\xC0" + frame
    got = []
    for i in range(0, len(stream), 3):
        got.extend(parser.feed(stream[i:i + 3]))
    if len(got) == 1 and fbgrab.ppm_bytes(*_swap(got[0])) == expected:
        print("  resynchronises past junk and across chunk boundaries")
    else:
        print(f"  FAIL resync produced {len(got)} frames")
        fails += 1

    # A realistic 128x128 frame must round-trip too.
    big = bytes((i * 7) & 0xFF for i in range(128 * 128 * 2))
    bigframe = b"FBUF" + bytes([128, 0, 128, 0]) + big
    parser = fbgrab.FrameParser()
    got = list(parser.feed(bigframe))
    if len(got) == 1 and got[0][0] == 128 and got[0][1] == 128 and got[0][2] == big:
        print("  128x128 frame round-trips")
    else:
        print("  FAIL 128x128 frame did not round-trip")
        fails += 1

    return fails


def _swap(frame_tuple):
    """(w, h, pixels) -> (pixels, w, h) for ppm_bytes' argument order."""
    w, h, pixels = frame_tuple
    return (pixels, w, h)


SUITE_HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include "chipgate.h"
#include "handshake.h"
int main(int argc, char **argv) {
    int send_chips = (argc > 1) ? atoi(argv[1]) : 1;
    int frames     = (argc > 2) ? atoi(argv[2]) : 4000;
    chipgate_t cg; chipgate_init(&cg, CG_GAME_BN6_US);
    hs_t h; hs_init(&h, HS_GATE_ID_BN6US); hs_enable_relink(&h);
    uint16_t stage = HS_WORD_IDLE;
    unsigned transfers = 0, sent = 0, ready = 0;
    for (int i = 0; i < frames; i++) {
        uint16_t answer = chipgate_step(&cg, stage);
        stage = hs_step(&h, answer);
        transfers++;
        if (chipgate_take_consumed(&cg)) ready = transfers + 20;
        if (send_chips && chipgate_connected(&cg) && !chipgate_busy(&cg)) {
            if (ready == 0) ready = transfers + 40;
            if (sent < 2 && transfers >= ready) {
                if (chipgate_load(&cg, 0x0101)) { sent++; ready = 0; }
            }
        }
    }
    printf("%08X %u %u\n", h.tests, h.chip_events, h.chip_rotations);
    return 0;
}
"""


def test_suite(tmp, root):
    """The ROM's six-check suite, driven against the real chipgate.c."""
    src, exe = os.path.join(tmp, "s.c"), os.path.join(tmp, "s")
    with open(src, "w") as fh:
        fh.write(SUITE_HARNESS)
    subprocess.run(["cc", "-std=c11", "-I", os.path.join(root, "src"),
                    "-I", os.path.join(root, "rom"), "-o", exe, src,
                    os.path.join(root, "src", "chipgate.c")], check=True)

    fails = 0
    out = subprocess.run([exe, "1", "4000"], capture_output=True,
                         text=True, check=True).stdout.split()
    bits, chips, reps = int(out[0], 16), int(out[1]), int(out[2])

    names = ["link", "shape", "counters", "chip", "repeats", "relink"]
    for i, n in enumerate(names):
        if not (bits & (1 << i)):
            print(f"  FAIL {n} did not pass against the real gate")
            fails += 1
    if chips != 2:
        print(f"  FAIL expected 2 chip deliveries, saw {chips}")
        fails += 1
    if reps != 9:
        print(f"  FAIL expected exactly 9 repeats, saw {reps}")
        fails += 1
    if not fails:
        print(f"  all six checks pass; {chips} chips, {reps} repeats each")

    # A suite that cannot fail proves nothing. With no chip sent, exactly the
    # two firmware-dependent checks must go red and the other four stay green.
    out2 = subprocess.run([exe, "0", "4000"], capture_output=True,
                          text=True, check=True).stdout.split()
    bits2 = int(out2[0], 16)
    if bits2 & 0x18:
        print("  FAIL chip checks passed with no chip sent")
        fails += 1
    elif (bits2 & 0x27) != 0x27:
        print("  FAIL protocol checks regressed when no chip was sent")
        fails += 1
    else:
        print("  control: no chip sent -> only the two chip checks fail")
    return fails


def main() -> int:
    fails = (test_handshake() + test_chip_injection()
             + test_session_lifecycle() + test_framebuffer())
    with tempfile.TemporaryDirectory() as tmp:
        print("\nthe six-check suite against the real chipgate.c")
        g_fail_extra = test_suite(tmp, ROOT)
        fails += g_fail_extra

    print("\n" + ("FAILURES" if fails else
                  "the ROM handshake drives the real chipgate.c, a slotted chip "
                  "goes out over the full rotation, sessions tear down and "
                  "relink, and the framebuffer path matches the render test"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
