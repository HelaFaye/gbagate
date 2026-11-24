"""la_check.py -- check a logic analyzer's capture of the RMT link test's burst.

    python3 tools/la_check.py capture.vcd [--sc D0] [--so D1] [--mark D4]

The capture is a VCD file (PulseView: File > Export > Value Change Dump; or
sigrok-cli -O vcd), holding one burst: GPIO1 (the marker) high, then the
Multi-Player frames and the normal-mode words of main/rl_patterns.h, in order.
The firmware's own loopback cannot see a clock error (it sends and receives
on the ESP32's clock); the analyzer's clock is another. This checks:

  - every Multi-Player frame decodes to the word sent; the bit time, fitted
    over every edge of every frame, is within 1% of 115200 baud's; and each
    edge is within a sample and a half of that fit (jitter);
  - every normal-mode word, at 256 kHz and 2 MHz, reads back (SO at SC's
    rising edges); SC's period is within 1% on average, every half period
    within a sample and a half of nominal; and SO changes with SC's fall,
    within a sample and a half (the sync manager's skew).

At 24 MHz a sample is 41.7 ns: a 2 MHz half period is 6 samples, so single
edges are only good to a sample; averages are better.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PATTERNS = os.path.join(HERE, '..', 'main', 'rl_patterns.h')
MP_NS = 8680.0


def patterns(path=PATTERNS):
    text = open(path).read()

    def array(name):
        body = re.search(r'%s\[\]\s*=\s*\{([^}]*)\}' % name, text).group(1)
        body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
        return [int(v, 0) for v in re.findall(r'0x[0-9A-Fa-f]+|\d+', body)]
    return array('RL_MP_WORDS'), array('RL_NORMAL_WORDS'), array('RL_NORMAL_BIT_NS')


def read_vcd(path):
    """{name: [(t_ns, level), ...]} from a VCD of single-bit wires."""
    ids, names, scale = {}, {}, 1.0
    units = {'s': 1e9, 'ms': 1e6, 'us': 1e3, 'ns': 1.0, 'ps': 1e-3, 'fs': 1e-6}
    text = open(path).read()
    head, _, body = text.partition('$enddefinitions')
    m = re.search(r'\$timescale\s*(\d+)\s*(\w+)\s*\$end', head)
    if m:
        scale = int(m.group(1)) * units[m.group(2)]
    for m in re.finditer(r'\$var\s+\w+\s+1\s+(\S+)\s+(\S+)', head):
        ids[m.group(1)] = m.group(2)
        names[m.group(2)] = []
    t = 0.0
    for tok in body.split():
        if tok.startswith('#'):
            t = int(tok[1:]) * scale
        elif tok[0] in '01' and tok[1:] in ids:
            seq = names[ids[tok[1:]]]
            level = int(tok[0])
            if not seq or seq[-1][1] != level:
                seq.append((t, level))
    return names


def edges(seq, t0, t1):
    return [(t, l) for t, l in seq if t0 <= t <= t1]


def level_at(seq, t):
    level = seq[0][1] if seq else 1
    for tt, l in seq:
        if tt > t:
            break
        level = l
    return level


def check(capture, sc_name, so_name, mark_name, sample_ns=1e9 / 24e6, out=print):
    mp_words, normal_words, bit_ns = patterns()
    sig = read_vcd(capture)
    for n in (sc_name, so_name, mark_name):
        if n not in sig:
            out('FAIL  no channel %r in the capture (it has %s)' % (n, ', '.join(sorted(sig))))
            return 1
    mark = sig[mark_name]
    rise = next((t for t, l in mark if l == 1), None)
    fall = next((t for t, l in mark if l == 0 and rise is not None and t > rise), None)
    if rise is None or fall is None:
        out('FAIL  no whole burst: the marker (%s) must rise and fall in the capture' % mark_name)
        return 1
    sc, so = edges(sig[sc_name], rise, fall), edges(sig[so_name], rise, fall)
    fails = 0

    def report(ok, what):
        nonlocal fails
        out('%s  %s' % ('ok  ' if ok else 'FAIL', what))
        fails += 0 if ok else 1

    # ---- Multi-Player: SO frames before SC's first edge
    first_sc = sc[0][0] if sc else fall
    so_mp = [e for e in so if e[0] < first_sc]
    frames, cur = [], []
    for t, l in so_mp:
        if l == 0 and (not cur or t - cur[-1][0] > 150000) and (not cur or cur[-1][1] == 1):
            if cur:
                frames.append(cur)
            cur = []
        cur.append((t, l))
    if cur:
        frames.append(cur)
    got, pairs = [], []
    for f in frames:
        start = f[0][0]
        bits = [0] * 18
        for i, (t, l) in enumerate(f):
            pos = (t - start) / MP_NS
            k = round(pos)
            pairs.append((k, t - start))
            end = round((f[i + 1][0] - start) / MP_NS) if i + 1 < len(f) else 18
            for b in range(k, min(end, 18)):
                bits[b] = l
        w = sum(bits[1 + j] << j for j in range(16)) if bits[0] == 0 and bits[17] == 1 else None
        got.append(w)
    report(got == mp_words, 'Multi-Player: %d frames, the words sent (%s)' % (
        len(got), ' '.join('%04X' % w if w is not None else '????' for w in got)))
    pairs = [(k, t) for k, t in pairs if k > 0]
    bit = sum(k * t for k, t in pairs) / max(sum(k * k for k, _ in pairs), 1)   # least squares, through the start
    jitter = max((abs(t - k * bit) for k, t in pairs), default=0.0)
    report(abs(bit / MP_NS - 1) <= 0.01,
           '... the bit time, over %d edges: %.1f ns (115200 baud: %.1f; %+.2f%%)' % (
               len(pairs), bit, MP_NS, 100 * (bit / MP_NS - 1)))
    report(jitter <= 1.5 * sample_ns, '... every edge within %.0f ns of it (worst %.0f; a sample is %.1f ns)' % (
        1.5 * sample_ns, jitter, sample_ns))

    # ---- normal mode: words of 32 SC falls
    falls = [t for t, l in sc if l == 0]
    rises = [t for t, l in sc if l == 1]
    groups, cur = [], []
    for t in falls:
        if cur and t - cur[-1] > 20000:
            groups.append(cur)
            cur = []
        cur.append(t)
    if cur:
        groups.append(cur)
    expect = [(b, w) for b in bit_ns for w in normal_words]
    report(len(groups) == len(expect) and all(len(g) == 32 for g in groups),
           'normal mode: %d words of 32 clocks (%s)' % (len(groups), ', '.join(str(len(g)) for g in groups)))
    for (b, w), g in zip(expect, groups):
        g_rises = [r for r in rises if g[0] < r < g[-1] + b]
        word = 0
        for r in g_rises[:32]:
            word = word << 1 | level_at(sig[so_name], r)
        period = (g[-1] - g[0]) / 31
        halves = []
        for f in g:
            r = next((x for x in g_rises if x > f), None)
            if r is not None:
                halves.append(r - f)
        half_err = max(abs(h - b / 2) for h in halves) if halves else 1e9
        so_edges = [t for t, l in edges(sig[so_name], g[0] - b, g[-1] + b)]
        skew = max((min(abs(t - f) for f in g) for t in so_edges[:-1]), default=0.0)   # the last: back to idle
        report(word == w, '%s %08X: read %08X' % ('256 kHz' if b > 1000 else '2 MHz  ', w, word))
        report(abs(period - b) <= 0.01 * b and half_err <= 1.5 * sample_ns and skew <= 1.5 * sample_ns,
               '... period %.1f ns (nominal %d), half periods within %.0f ns, SO to SC skew %.0f ns' % (
                   period, b, half_err, skew))
    out('la_check: %s' % ('all checks passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('capture')
    ap.add_argument('--sc', default='D0', help='the channel on GPIO4 (SC)')
    ap.add_argument('--so', default='D1', help='the channel on GPIO5 (SO, SD)')
    ap.add_argument('--mark', default='D4', help='the channel on GPIO1 (the marker)')
    ap.add_argument('--rate', type=float, default=24e6, help='the capture\'s sample rate, Hz')
    a = ap.parse_args()
    return check(a.capture, a.sc, a.so, a.mark, sample_ns=1e9 / a.rate)


if __name__ == '__main__':
    sys.exit(main())
