#!/usr/bin/env python3
"""Sample analysis for the PSG arranger: what each instrument of a PSM song
sounds like, measured from the sample data itself.

  analyse(sample) -> Info(kind, root, conf, flat, bright, chord)

kind    "tone" (pitched), "drum" (noise-like) or "tom" (pitched, short, falling)
root    MIDI note heard when the sample plays at its c5 rate (float)
conf    0..1, how harmonic the spectrum is around that root
flat    spectral flatness 0..1 (1 = white noise)
bright  spectral centroid in Hz at the c5 rate
chord   None, "major", "minor" or "power" when the sample is a chord

The root comes from a two-way mismatch fit (Maher & Beauchamp) over the
strongest spectral peaks of the sample's steady part (its loop, if it has
one), which unlike autocorrelation does not lock onto the fifth or the octave
above or below."""
import math
import numpy as np

NFFT = 1 << 16


def _steady(s):
    d = np.asarray(s.data, float)
    d = d - d.mean()
    if s.loop and s.loop_end - s.loop_start >= 64:
        lp = d[s.loop_start:s.loop_end]
        reps = max(1, 16384 // len(lp) + 1)
        seg = np.tile(lp, reps)[:16384]
        return seg
    # skip the attack: start a little after the loudest part
    n = len(d)
    w = max(64, n // 64)
    env = np.convolve(d * d, np.ones(w) / w, "same")
    pk = int(np.argmax(env))
    a = min(n - 256, pk + w) if n > 512 else 0
    seg = d[max(0, a):max(0, a) + 16384]
    if len(seg) < 512:
        seg = d[:16384]
    return seg


def _spectrum(s):
    seg = _steady(s)
    if len(seg) < 64 or not np.any(seg):
        return None, None
    win = np.hanning(len(seg))
    sp = np.abs(np.fft.rfft(seg * win, NFFT))
    fr = np.fft.rfftfreq(NFFT, 1.0 / s.c5)
    return fr, sp


def _peaks(fr, sp, fmin=30.0, fmax=6000.0, k=16):
    ok = (fr > fmin) & (fr < min(fmax, fr[-1] * 0.95))
    idx = np.nonzero(ok)[0]
    if len(idx) < 3:
        return []
    s = sp[idx]
    loc = (s[1:-1] > s[:-2]) & (s[1:-1] >= s[2:])
    cand = idx[1:-1][loc]
    if not len(cand):
        return []
    top = sp[cand].max()
    cand = cand[sp[cand] > top * 0.02]
    cand = cand[np.argsort(-sp[cand])][:k]
    out = []
    for c in cand:
        # parabolic interpolation of the peak frequency
        a, b, g = math.log(sp[c - 1] + 1e-9), math.log(sp[c] + 1e-9), math.log(sp[c + 1] + 1e-9)
        p = 0.5 * (a - g) / (a - 2 * b + g) if (a - 2 * b + g) else 0.0
        out.append((fr[c] + p * (fr[1] - fr[0]), sp[c] / top))
    return out


def _twm(peaks, f0, p=0.5, q=1.4, r=0.5, rho=0.33):
    """Two-way mismatch error of a fundamental against the measured peaks
    (Maher & Beauchamp 1994, their constants)."""
    pf = np.array([f for f, a in peaks])
    pa = np.array([a for f, a in peaks])
    fmax = pf.max()
    nh = max(1, min(30, int(fmax / f0 + 0.5)))
    fh = f0 * np.arange(1, nh + 1)
    # predicted -> measured
    j = np.argmin(np.abs(pf[None, :] - fh[:, None]), axis=1)
    df = np.abs(pf[j] - fh)
    t = df * fh ** -p
    e1 = np.sum(t + pa[j] * (q * t - r)) / nh
    # measured -> predicted
    h = np.maximum(1, np.round(pf / f0))
    df = np.abs(pf - h * f0)
    t = df * pf ** -p
    e2 = np.sum(t + pa * (q * t - r)) / len(pf)
    return e1 + rho * e2


def _root(peaks):
    if len(peaks) < 1:
        return None, 0.0
    cands = set()
    for f, a in peaks[:8]:
        for h in range(1, 9):
            f0 = f / h
            if 27.0 <= f0 <= 2500.0:
                cands.add(f0)
    best = None
    for f0 in sorted(cands):
        e = _twm(peaks, f0)
        if best is None or e < best[0] - 1e-9:
            best = (e, f0)
    e, f0 = best
    # refine: least-squares fundamental from the matched peaks
    num = den = 0.0
    for f, a in peaks:
        h = round(f / f0)
        if h >= 1 and abs(f - h * f0) / f < 0.03:
            num += a * h * f
            den += a * h * h
    if den:
        f0 = num / den
    # how much of the peak energy the harmonic series explains
    expl = sum(a for f, a in peaks if abs(f - max(1, round(f / f0)) * f0) / f < 0.03)
    conf = expl / sum(a for f, a in peaks)
    return 69 + 12 * math.log2(f0 / 440.0), conf


def _flatness(sp):
    s = sp[(sp > 0)]
    s = s[: len(s) // 2]
    if not len(s):
        return 1.0
    return float(np.exp(np.mean(np.log(s + 1e-9))) / (np.mean(s) + 1e-9))


CHORDS = {"major": (0, 4, 7), "minor": (0, 3, 7), "power": (0, 7)}


def _chord(peaks, root):
    """Chord type when the strong peaks fall on two or three roots."""
    if root is None:
        return None
    pcs = {}
    for f, a in peaks:
        m = 69 + 12 * math.log2(f / 440.0)
        pc = round(m - root) % 12
        if abs((m - root) - round(m - root)) < 0.3:
            pcs[pc] = pcs.get(pc, 0) + a
    tot = sum(pcs.values()) or 1
    def has(i):
        return pcs.get(i, 0) / tot > 0.06
    if has(4) and has(7):
        return "major"
    if has(3) and has(7):
        return "minor"
    return None


class Info:
    def __init__(self, **k):
        self.__dict__.update(k)

    def __repr__(self):
        r = "%.2f" % self.root if self.root is not None else "-"
        return "%-4s root %6s conf %.2f flat %.3f bright %5.0f len %.2fs%s" % (
            self.kind, r, self.conf, self.flat, self.bright, self.secs,
            (" " + self.chord) if self.chord else "")


def analyse(s):
    fr, sp = _spectrum(s)
    secs = len(s.data) / float(s.c5)
    if fr is None:
        return Info(kind="drum", root=None, conf=0.0, flat=1.0, bright=0.0, chord=None, secs=secs)
    peaks = _peaks(fr, sp)
    root, conf = _root(peaks)
    flat = _flatness(sp[(fr > 50) & (fr < min(8000, s.c5 / 2))])
    pw = sp ** 2
    bright = float((fr * pw).sum() / (pw.sum() or 1))
    chord = _chord(peaks, root) if conf > 0.4 else None
    if conf < 0.55 or flat > 0.25:
        kind = "drum"
    else:
        kind = "tone"
    return Info(kind=kind, root=root, conf=conf, flat=flat, bright=bright, chord=chord, secs=secs)


if __name__ == "__main__":
    import os, sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import psm
    for p in sys.argv[1:]:
        m = psm.Module(p)
        print(os.path.basename(p))
        for k, s in sorted(m.samples.items()):
            if len(s.data) > 16:
                print("  %-26s %s" % (s.name[:26], analyse(s)))


def contour(s, root, hop_frames=120.0):
    """The sample's own pitch movement (bends, drops, vibrato baked into the
    recording), as (sample positions, semitones relative to root), tracked by
    YIN within a fifth of the root.  None when the pitch stays put."""
    d = np.asarray(s.data, float)
    d = d - d.mean()
    f_root = 440.0 * 2 ** ((root - 69) / 12.0)
    t0 = s.c5 / f_root
    tmin = max(2, int(t0 / 2 ** (7 / 12.0)))
    tmax = int(t0 * 2 ** (7 / 12.0)) + 2
    W = max(256, int(2.5 * tmax))
    hop = max(32, int(s.c5 / hop_frames))
    end = s.loop_end if s.loop else len(d)
    if end < W + tmax + 2:
        return None
    pos, dev, ok = [], [], []
    taus = np.arange(tmin, tmax + 1)
    for a in range(0, end - W - tmax - 1, hop):
        x = d[a:a + W]
        e = np.dot(x, x)
        if e <= 1e-6 * W:
            pos.append(a + W // 2); dev.append(0.0); ok.append(False)
            continue
        # difference function d(tau) for the candidate lags
        df = np.array([np.sum((x - d[a + t:a + t + W]) ** 2) for t in taus])
        # cumulative-mean normalisation over the lags (relative, YIN style)
        e2 = np.array([np.dot(d[a + t:a + t + W], d[a + t:a + t + W]) for t in taus])
        nd = df / (e + e2 + 1e-9)
        i = int(np.argmin(nd))
        tau = float(taus[i])
        if 0 < i < len(nd) - 1:
            den = nd[i - 1] - 2 * nd[i] + nd[i + 1]
            if den > 0:
                tau += 0.5 * (nd[i - 1] - nd[i + 1]) / den
        pos.append(a + W // 2)
        dev.append(12 * math.log2(t0 / tau))
        ok.append(nd[i] < 0.25 and 0 < i < len(nd) - 1)
    if not any(ok):
        return None
    pos, dev, ok = np.array(pos, float), np.array(dev), np.array(ok)
    # unreliable windows take the nearest reliable value
    good = np.nonzero(ok)[0]
    for k in np.nonzero(~ok)[0]:
        dev[k] = dev[good[np.argmin(np.abs(good - k))]]
    # drop tracking slips (median of 5), then light smoothing (3 windows)
    if len(dev) >= 5:
        p = np.pad(dev, 2, mode="edge")
        dev = np.median(np.stack([p[i:i + len(dev)] for i in range(5)]), axis=0)
    if len(dev) >= 3:
        dev = np.convolve(np.pad(dev, 1, mode="edge"), np.ones(3) / 3, "valid")
    # movement only: the root already gives the pitch the sample settles on
    dev = dev - np.median(dev[ok]) if ok.sum() else dev - np.median(dev)
    if np.max(dev) - np.min(dev) < 0.6:
        return None
    return pos, dev
