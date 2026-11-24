"""test_la_check.py -- la_check.py against captures made here: a burst as the
firmware sends it, sampled at 24 MHz; then with a clock 3% fast, a wrong
word, and a skewed SO.

    python3 -I test/test_la_check.py
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
import la_check  # noqa: E402

SAMPLE = 1e9 / 24e6


def burst(clock=1.0, mp_override=None, skew_ns=0.0):
    """Edges {name: [(t_ns, level)]}: the marker, the MP frames, the normal words."""
    mp, normal, bits = la_check.patterns()
    if mp_override:
        mp = [mp_override.get(i, w) for i, w in enumerate(mp)]
    sc, so, mark = [(0, 1)], [(0, 1)], [(0, 0)]
    t = 10000.0
    mark.append((t, 1))
    t += 5000
    for w in mp:
        levels = [0] + [w >> j & 1 for j in range(16)] + [1]
        for k, l in enumerate(levels):
            if so[-1][1] != l:
                so.append((t + k * 8680 * clock, l))
        t += 18 * 8680 * clock + 300000
    t += 1000000
    for b in bits:
        for w in normal:
            for k in range(32):
                f = t + k * b * clock
                sc.append((f, 0))
                sc.append((f + b / 2 * clock, 1))
                l = w >> (31 - k) & 1
                if so[-1][1] != l:
                    so.append((f + skew_ns, l))
            if so[-1][1] != 1:
                so.append((t + 32 * b * clock, 1))
            t += 32 * b * clock + 300000
        t += 1000000
    mark.append((t, 0))
    return {'D0': sc, 'D1': so, 'D4': mark}, t + 10000


def write_vcd(path, sig, end):
    """At 24 MHz: every edge on a sample (a real analyzer's quantization)."""
    ids = {'D0': '!', 'D1': '"', 'D4': '#'}
    events = sorted((round(t / SAMPLE), ids[n], l) for n, seq in sig.items() for t, l in seq)
    with open(path, 'w') as f:
        f.write('$timescale 1 ns $end\n$scope module test $end\n')
        for n, i in ids.items():
            f.write('$var wire 1 %s %s $end\n' % (i, n))
        f.write('$upscope $end\n$enddefinitions $end\n')
        last = None
        for s, i, l in events:
            ns = round(s * SAMPLE)
            if ns != last:
                f.write('#%d\n' % ns)
                last = ns
            f.write('%d%s\n' % (l, i))
        f.write('#%d\n' % round(end))


fails = 0


def run(name, sig, end, want):
    global fails
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'c.vcd')
        write_vcd(p, sig, end)
        lines = []
        r = la_check.check(p, 'D0', 'D1', 'D4', out=lines.append)
    ok = (r == 0) == want
    fails += 0 if ok else 1
    print('%s  %s: la_check %s' % ('ok  ' if ok else 'FAIL', name, 'passes' if r == 0 else 'fails'))
    if not ok or os.environ.get('VERBOSE'):
        print('\n'.join('      ' + l for l in lines))
    return lines


lines = run('a burst as sent, at 24 MHz', *burst(), want=True)
run('the ESP32 clock 3% fast', *burst(clock=0.97), want=False)
run('one Multi-Player word wrong', *burst(mp_override={4: 0xFFFF}), want=False)
run('SO 100 ns late after SC falls', *burst(skew_ns=100), want=False)
run('the ESP32 clock 0.3% fast: within the margins', *burst(clock=0.997), want=True)
run('the ESP32 clock 1.5% slow', *burst(clock=1.015), want=False)
print('test_la_check: %s' % ('all passed' if not fails else '%d FAILED' % fails))
sys.exit(1 if fails else 0)
