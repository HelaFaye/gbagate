"""test_bench.py -- tools/gblink_bench.py, the bring-up script, against the
simulator: each step with the GBA it is meant for behind it, so a step that
says "ok" on the bench has been seen to say it for the right reason here.

    python3 -I test/gblink_sim/test_bench.py
"""
import io
import os
import sys
import threading
import time
import traceback
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))
import gblink_bench as gb  # noqa: E402

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def check(cond, what):
    if not cond:
        raise AssertionError(what)


def run(step, port, args=(), during=None):
    """Run a step, capturing what it prints; `during` runs alongside it."""
    out = io.StringIO()
    t = threading.Thread(target=during) if during else None
    with redirect_stdout(out):
        if t:
            t.start()
        rc = gb.STEPS[step](gb.Board(port), list(args))
        if t:
            t.join()
    return rc, out.getvalue()


@test
def bench_info():
    p = gb.SimPort()
    try:
        rc, out = run('info', p)
        check(rc == 0, out)
        check('GB-Link protocol 2.2.7' in out, out)
        check('gbagate 0.4.0, host simulator, gate image' in out, out)
        check('link counters: mp_words' in out, out)
    finally:
        p.close()


@test
def bench_gen3_with_a_child_gba():
    p = gb.SimPort()
    try:
        p.gba(0x06, 1)
        p.gba(0x01, 1)               # a Gen 3 game, waiting as child
        rc, out = run('gen3', p)
        check(rc == 0 and 'HandshakeReceived' in out, out)
    finally:
        p.close()


@test
def bench_gen3_with_nothing_plugged_in():
    p = gb.SimPort()
    try:
        p.gba(0x01, 0)
        # The step itself waits 20 s for a GBA; the same sequence, shorter.
        b = gb.Board(p)
        m, _ = b.mark()
        b.cmd(0x00, 0x01)
        check(b.wait_status(0xFF02, 3, m), 'AwaitMode')
        b.cmd(0x10)
        check(not b.wait_status(0xFF03, 1.5, m), 'no handshake without a GBA')
        b.cancel()
    finally:
        p.close()


@test
def bench_rfu_sees_the_game_find_the_adapter():
    p = gb.SimPort()
    try:
        def game():
            time.sleep(0.5)
            p.gba(0x20, 8)           # librfu: AgbRFU_SoftReset + AgbRFU_checkID
        rc, out = run('rfu', p, ['8'], during=game)
        if any(c == 11 and b[:1] == b'\x00' for c, b in p.events):
            print('  (librfu not built in: fetch_librfu.sh; rfu step not exercised)')
            return
        check(rc == 0 and 'found the adapter' in out, out)
        check('transfer path: worst pass' in out, out)
    finally:
        p.close()


@test
def bench_gate_with_a_battle_network_game():
    p = gb.SimPort()
    try:
        p.gba(0x01, 4)               # MMBN polling a gate
        rc, out = run('gate', p, ['4', '0x012A'])
        check(rc == 0 and 'session open' in out and 'accepted' in out, out)
        check('chip sent' in out, out)
    finally:
        p.close()


def main(argv):
    fails = 0
    for t in TESTS:
        t0 = time.time()
        try:
            t()
            print('  ok    %-40s %5.2fs' % (t.__name__, time.time() - t0))
        except Exception:
            fails += 1
            print('  FAIL  %s' % t.__name__)
            traceback.print_exc()
    print('bench          %s' % ('all %d checks passed' % len(TESTS) if not fails else '%d of %d FAILED' % (fails, len(TESTS))))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
