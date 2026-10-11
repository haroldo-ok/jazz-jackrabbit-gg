#!/usr/bin/env python3
"""WAV previews of a PSM song: the original 4-channel sample mix (from the
tracker model) and the Game Gear PSG arrangement (from the player model),
plus a chroma (pitch-class) similarity score between the two.
Usage: musicpreview.py SONG.PSM outdir [seconds]"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psm, psgmusic

RATE = 22050


def mix_original(mod, ticks, seconds):
    out = []
    pos_map = {}
    for secs, state in ticks:
        n = int(round(secs * RATE))
        mix = np.zeros(n)
        for ch, st in enumerate(state):
            if st is None:
                continue
            smp, f, v, pos = st[:4]
            s = mod.samples[smp]
            d = np.array(s.data, float)
            p = pos + f / RATE * np.arange(n)
            if s.loop:
                over = p >= s.loop_end
                p[over] = s.loop_start + (p[over] - s.loop_start) % (s.loop_end - s.loop_start)
            ok = p < len(d)
            idx = np.floor(p[ok]).astype(int)
            mix[:ok.sum()] += d[idx] * v / 64.0
        out.append(mix)
        if sum(len(o) for o in out) > seconds * RATE:
            break
    return np.concatenate(out)[:int(seconds * RATE)]


def chroma(buf, rate, hop=0.25):
    win = int(rate * 0.25)
    frames = []
    freqs = np.fft.rfftfreq(win, 1.0 / rate)
    ok = (freqs > 100) & (freqs < 2000)
    pc = (np.round(12 * np.log2(freqs[ok] / 440.0)) % 12).astype(int)
    for i in range(0, len(buf) - win, int(rate * hop)):
        sp = np.abs(np.fft.rfft(buf[i:i + win] * np.hanning(win)))[ok]
        c = np.bincount(pc, weights=sp ** 2, minlength=12)
        frames.append(c / (np.linalg.norm(c) or 1))
    return np.array(frames)


def main():
    path, outdir = sys.argv[1], sys.argv[2]
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 60
    name = os.path.splitext(os.path.basename(path))[0].lower()
    os.makedirs(outdir, exist_ok=True)
    mod = psm.Module(path)
    ticks, _ = psm.play(mod)
    orig = mix_original(mod, ticks, seconds)
    psgmusic.write_wav(orig, os.path.join(outdir, name + "_original.wav"), RATE)
    blob, info = psgmusic.song_blob(path)
    writes = psgmusic.reference_writes(blob, int(seconds * 60))
    gg = psgmusic.synth(writes, RATE)
    psgmusic.write_wav(gg, os.path.join(outdir, name + "_gamegear.wav"), RATE)
    a, b = chroma(orig, RATE), chroma(gg, RATE)
    n = min(len(a), len(b))
    sim = np.sum(a[:n] * b[:n], axis=1)
    # the same with the GG version shifted by k semitones (sanity: k=0 should win)
    shifts = [np.mean(np.sum(a[:n] * np.roll(b[:n], k, axis=1), axis=1)) for k in range(12)]
    print("%s: %s  chroma similarity %.3f (best shift %d: %s)" % (
        name, info, sim.mean(), int(np.argmax(shifts)), " ".join("%.2f" % x for x in shifts)))


if __name__ == "__main__":
    main()
