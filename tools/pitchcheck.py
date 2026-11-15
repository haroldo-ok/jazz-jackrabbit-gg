#!/usr/bin/env python3
"""Per-instrument pitch check: for every pitched sample of a song, the
original song's notes on that sample alone are mixed from the samples, and the
same notes are synthesised as sawtooth waves (all harmonics, so the
fundamental's pitch class dominates as it does to the ear) at the pitch the
arranger gives them.  The chroma (pitch-class) similarity of the two, and the semitone
shift that would make it best, show whether the sample's root was measured
right.  Usage: pitchcheck.py SONG.PSM... [--seconds N]"""
import math, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psm, smpanalysis

RATE = 11025
WIN = 4096


def chroma_frames(buf):
    freqs = np.fft.rfftfreq(WIN, 1.0 / RATE)
    ok = (freqs > 30) & (freqs < 2500)
    pc = (np.round(12 * np.log2(freqs[ok] / 440.0)) % 12).astype(int)
    out, en = [], []
    hann = np.hanning(WIN)
    for i in range(0, len(buf) - WIN, WIN // 2):
        sp = np.abs(np.fft.rfft(buf[i:i + WIN] * hann))[ok] ** 2
        c = np.bincount(pc, weights=sp, minlength=12)
        en.append(c.sum())
        out.append(c / (np.linalg.norm(c) or 1))
    return np.array(out), np.array(en)


def renders(mod, ticks, smp, root, seconds, chord=(0,)):
    """(original mix of sample smp, square waves at root) as arrays."""
    s = mod.samples[smp]
    d = np.asarray(s.data, float)
    d = d / (np.abs(d).max() or 1)
    o_parts, g_parts = [], []
    total = 0
    phase = {}
    for secs, st in ticks:
        n = int(round(secs * RATE))
        a = np.zeros(n)
        g = np.zeros(n)
        for ch, c in enumerate(st):
            if c is None or c[0] != smp:
                continue
            _, f, v, pos = c[:4]
            p = pos + f / RATE * np.arange(n)
            if s.loop:
                over = p >= s.loop_end
                p[over] = s.loop_start + (p[over] - s.loop_start) % (s.loop_end - s.loop_start)
            ok = p < len(d)
            idx = np.floor(p[ok]).astype(int)
            seg = d[idx] * v / 64.0
            a[:ok.sum()] += seg
            # square wave at the arranged pitch, following the sample's level
            hz = 440.0 * 2 ** ((root + 12 * math.log2(f / s.c5) - 69) / 12.0)
            ph = phase.get(ch, 0.0) + hz / RATE * np.arange(n)
            phase[ch] = (ph[-1] + hz / RATE) % 1.0
            lvl = np.sqrt(np.convolve(seg * seg, np.ones(64) / 64, "same")) if len(seg) else seg
            sq = sum(2.0 * ((ph * 2 ** (o / 12.0)) % 1.0) - 1.0 for o in chord)[:len(lvl)] * lvl
            g[:len(sq)] += sq
        o_parts.append(a); g_parts.append(g)
        total += n
        if total > seconds * RATE:
            break
    return np.concatenate(o_parts), np.concatenate(g_parts)


def check(mod, ticks, smp, root, seconds=60, chord=(0,)):
    o, g = renders(mod, ticks, smp, root, seconds, chord)
    A, ea = chroma_frames(o)
    B, eb = chroma_frames(g)
    n = min(len(A), len(B))
    if n == 0:
        return None
    live = ea[:n] > ea[:n].max() * 0.01
    if not live.any():
        return None
    sims = [float(np.mean(np.sum(A[:n][live] * np.roll(B[:n][live], k, axis=1), axis=1))) for k in range(12)]
    return sims


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    seconds = 60
    for p in args:
        mod = psm.Module(p)
        ticks, _ = psm.play(mod)
        used = {c[0] for _, st in ticks for c in st if c is not None}
        print(os.path.basename(p))
        for k in sorted(used):
            s = mod.samples[k]
            inf = smpanalysis.analyse(s)
            if inf.root is None:
                continue
            sims = check(mod, ticks, k, inf.root, seconds)
            if sims is None:
                continue
            best = int(np.argmax(sims))
            print("  %-24s %s  sim %.2f best %+d (%.2f)" % (s.name[:24], inf, sims[0],
                                                          best if best < 7 else best - 12, sims[best]))


if __name__ == "__main__":
    main()
