#!/usr/bin/env python3
import sys, math, collections
sys.path.insert(0, "tools")
import psm
import numpy as np

NAMES = ["C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"]
def nn(n): return "%s%d" % (NAMES[(n - 1) % 12], (n - 1) // 12)

def pitch(s):
    """Fundamental (cycles per sample frame) via autocorrelation on a steady window."""
    d = np.array(s.data, dtype=float)
    if len(d) < 600: return None, 0
    start = s.loop_start if s.loop and s.loop_end - s.loop_start > 600 else min(len(d) // 5, 2000)
    w = d[start:start + 4096]
    if len(w) < 600: w = d[:4096]
    w = w - w.mean()
    if not w.any(): return None, 0
    ac = np.correlate(w, w, "full")[len(w) - 1:]
    ac /= ac[0]
    lo = 8
    hi = min(len(w) // 2, 1200)
    # first strong peak
    best, bv = None, 0
    for i in range(lo, hi):
        if ac[i] > ac[i - 1] and ac[i] >= ac[i + 1] and ac[i] > 0.5:
            best, bv = i, ac[i]
            break
    if best is None: return None, float(ac[lo:hi].max())
    # parabolic refine
    a, b, c = ac[best - 1], ac[best], ac[best + 1]
    off = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) else 0
    return 1.0 / (best + off), bv

for f in sys.argv[1:]:
    m = psm.Module(f)
    print("=====", f, "speed", m.speed, "tempo", m.tempo)
    use = collections.defaultdict(lambda: collections.Counter())
    notes = collections.defaultdict(list)
    cur = {}
    for pi in m.order:
        for row in m.patterns[pi]:
            for ch, c in row.items():
                if c.instr: cur[ch] = c.instr
                if c.note and c.note < 120 and ch in cur:
                    use[ch][cur[ch]] += 1
                    notes[(ch, cur[ch])].append(c.note)
    for ch in sorted(use):
        print(" ch%d:" % ch, end="")
        for ins, n in use[ch].most_common():
            ns = notes[(ch, ins)]
            print("  S%02d x%d [%s..%s]" % (ins, n, nn(min(ns)), nn(max(ns))), end="")
        print()
    for s in sorted(m.samples.values(), key=lambda s: s.num):
        if len(s.data) < 100: continue
        cps, conf = pitch(s)
        if cps:
            f0 = cps * s.c5      # Hz when played at C-5
            midi = 69 + 12 * math.log2(f0 / 440)
            print("   S%02d %-14s f0@C5 %7.1f Hz (midi %.1f, conf %.2f) dur@C5 %.2fs" % (s.num, s.name, f0, midi, conf, len(s.data) / s.c5))
        else:
            print("   S%02d %-14s unpitched (best ac %.2f) dur@C5 %.2fs" % (s.num, s.name, conf, len(s.data) / s.c5))
