#!/usr/bin/env python3
"""
test_gate_drive.py -- prove the broker's --gate mode drives a real gate.

The broker's job in --gate mode is to act like a Battle Network game: sync
word, acknowledge, then idle frames. If it gets that sequence wrong the
firmware simply never links, and from outside that is indistinguishable from
the firmware being broken.

So the sequence is run here against the *actual* chipgate.c, compiled and
driven through a small harness. If this passes, a failure under QEMU is the
firmware's or the emulator's, not the broker's.

Needs a C compiler. No hardware, no ESP-IDF, no QEMU.
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lp  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HARNESS = r'''
/* stdin: one hex word per line, what the game sent. stdout: the gate's answer. */
#include <stdio.h>
#include <stdlib.h>
#include "chipgate.h"

int main(int argc, char **argv) {
    chipgate_t cg;
    cg_game_t game = (argc > 1) ? (cg_game_t)atoi(argv[1]) : CG_GAME_BN6_US;
    chipgate_init(&cg, game);

    char line[64];
    while (fgets(line, sizeof line, stdin)) {
        unsigned w = 0;
        if (sscanf(line, "%x", &w) != 1) continue;
        printf("%04X %d\n", chipgate_step(&cg, (uint16_t)w), (int)cg.state);
        fflush(stdout);
    }
    return 0;
}
'''

GATE_ID = {"bn4": (0xFFC6, 0), "bn5": (0xFFC7, 1),
           "bn6jp": (0xFFC4, 2), "bn6": (0xFF00, 3)}
SYNC = 0x8FFF


def build(tmp):
    src, exe = os.path.join(tmp, "g.c"), os.path.join(tmp, "g")
    open(src, "w").write(HARNESS)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra",
                    "-I", os.path.join(ROOT, "src"), "-o", exe, src,
                    os.path.join(ROOT, "src", "chipgate.c")], check=True)
    return exe


def drive(exe, game_key):
    """Replay the broker's --gate sequence against the real gate."""
    gate_id, game_idx = GATE_ID[game_key]

    proc = subprocess.Popen([exe, str(game_idx)], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, text=True)

    phase, frames, answers = "idle", 0, []
    outgoing = 0x0000

    for _ in range(60):
        proc.stdin.write(f"{outgoing:04X}\n")
        proc.stdin.flush()
        answer, state = proc.stdout.readline().split()
        answer = int(answer, 16)
        answers.append(answer)
        frames += 1

        # Exactly the logic in broker.run_gate()
        if phase == "idle" and answer == gate_id:
            phase = "announced"
        if phase == "announced" and frames > 2:
            outgoing, phase = SYNC, "synced"
        elif phase == "synced":
            outgoing, phase = gate_id, "linked"
        else:
            outgoing = 0x0000

    proc.stdin.close()
    proc.wait()
    return answers, int(state), phase


def main():
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)

        for key, (gate_id, _) in GATE_ID.items():
            answers, state, phase = drive(exe, key)

            # state 2 is CG_LINK_ACTIVE
            if state != 2:
                print(f"  FAIL {key}: never linked (state {state}, phase {phase})")
                fails += 1
                continue

            tail = answers[-18:]
            has_id = gate_id in tail
            has_ff = 0xFFFF in tail
            has_zero = 0x0000 in tail
            if not (has_id and has_ff and has_zero):
                print(f"  FAIL {key}: rotation looks wrong: "
                      + " ".join(f"{w:04X}" for w in tail))
                fails += 1
                continue

            print(f"  {key:5s} linked, id {gate_id:04X}, rotation "
                  + " ".join(f"{w:04X}" for w in answers[-9:]))

        # The sequence must be necessary, not incidental: a broker that never
        # sends the sync word must fail to link, or the test proves nothing.
        proc = subprocess.Popen([exe, "3"], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True)
        for _ in range(40):
            proc.stdin.write("0000\n")
            proc.stdin.flush()
            _, state = proc.stdout.readline().split()
        proc.stdin.close(); proc.wait()
        if int(state) == 2:
            print("  FAIL linked without ever sending the sync word")
            fails += 1
        else:
            print("  idle traffic alone does not link (as it should not)")

    print("\n" + ("FAILURES" if fails else
                  "the broker's gate sequence drives the real chipgate.c"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
