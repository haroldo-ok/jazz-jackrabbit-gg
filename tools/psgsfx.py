"""Sound effects for the PSG, converted from the original PCM clips.

The Genesis port plays the original SOUNDS.000 clips (and the four weapon
clips) through its PCM channels.  The Game Gear's PSG cannot play samples
during the game, so each clip is analysed frame by frame (1/60 s): its
loudness becomes the attenuation, its pitch (autocorrelation) the tone
period, and the frames where it has no clear pitch play on the noise
channel, brighter or darker by how often the wave crosses zero.

Effect stream (src/sound.c): 3 bytes a frame
    b0 = attenuation | 0x10 noise | 0x80 last frame
    tone:  b1, b2 = period (10 bits, little endian)
    noise: b1 = noise control (low 3 bits of the 0xE0 latch)
"""
import math
import os
import wave

import numpy as np

PSG_CLOCK = 3579545
FPS = 60
MAX_FRAMES = 96

# SOUNDS.000 order (1-based ids in the event records), then the weapons.
CLIPS = ["00_INVULN11", "01_MACHGUN1", "02_BOOM", "03_OW", "04_YUM1", "05_FIRE1",
         "06_UPLOOP11", "07_1UP", "08_PHOTON", "09_WAIT_12", "10_ORB10S", "11_JUMPA11",
         "12_GODLIKE", "13_YEAHOO", "14_BIRDY1", "15_FLAMER", "16_ELECTR11", "17_SPRING1",
         "18_ROCKET2", "19_STOP1", "20_BLOCK3", "21_BLOCK1",
         "fire_blaster", "fire_toaster", "fire_upward", "fire_bouncer"]
NAMES = ["INVULN", "MACHGUN", "BOOM", "OW", "YUM", "FIRE1", "UPLOOP", "ONEUP", "PHOTON",
         "WAIT", "ORB", "JUMP", "GODLIKE", "YEAHOO", "BIRDY", "FLAMER", "ELECTR", "SPRING",
         "ROCKET", "STOP", "BLOCK3", "BLOCK1",
         "SHOT_BLASTER", "SHOT_TOASTER", "SHOT_UPWARD", "SHOT_BOUNCER"]


def load(path):
    w = wave.open(path)
    n = w.getnframes()
    raw = w.readframes(n)
    rate = w.getframerate()
    width = w.getsampwidth()
    if width == 1:
        d = (np.frombuffer(raw, np.uint8).astype(float) - 128) / 128
    else:
        d = np.frombuffer(raw, np.int16).astype(float) / 32768
    if w.getnchannels() > 1:
        d = d.reshape(-1, w.getnchannels()).mean(1)
    return d, rate


def convert(path):
    d, rate = load(path)
    step = rate / FPS
    nfr = min(MAX_FRAMES, int(math.ceil(len(d) / step)))
    rms = []
    for k in range(nfr):
        a = d[int(k * step):int((k + 1) * step)]
        rms.append(math.sqrt(float(np.mean(a * a))) if len(a) else 0.0)
    peak = max(rms) or 1.0
    frames = []
    for k in range(nfr):
        r = rms[k]
        att = 15 if r <= 0 else int(round(-20 * math.log10(r / peak) / 2))
        att = max(0, min(15, att))
        c = int((k + 0.5) * step)
        win = d[max(0, c - 256):c + 256]
        win = win - win.mean() if len(win) else win
        pitch, conf, zc = None, 0.0, 0.0
        if len(win) > 64 and np.any(win):
            zc = float(np.mean(np.abs(np.diff(np.sign(win)))) / 2)
            n = len(win)
            f = np.fft.rfft(win, 2 * n)
            ac = np.fft.irfft(f * np.conj(f))[:n]
            ac /= ac[0]
            lo, hi = max(2, int(rate / 2500)), min(n - 2, int(rate / 70))
            if hi > lo + 2:
                k2 = int(np.argmax(ac[lo:hi])) + lo
                conf = float(ac[k2])
                pitch = rate / k2
        if pitch is not None and conf > 0.55:
            per = max(1, min(1023, int(round(PSG_CLOCK / (32.0 * pitch)))))
            frames.append((att, 0, per))
        else:
            ctrl = 0x04 if zc > 0.35 else 0x05 if zc > 0.15 else 0x06
            frames.append((att, 1, ctrl))
    while frames and frames[-1][0] == 15:
        frames.pop()
    if not frames:
        frames = [(15, 1, 4)]
    out = bytearray()
    for i, (att, noise, v) in enumerate(frames):
        b0 = att | (0x10 if noise else 0) | (0x80 if i == len(frames) - 1 else 0)
        out += bytes([b0, v & 0xFF, (v >> 8) & 0xFF])
    return bytes(out)


def build(out, clipdir):
    blobs = []
    for c in CLIPS:
        blobs.append(convert(os.path.join(clipdir, c + ".wav")))
    # one bank: the offset table then the streams
    table = 2 * (len(blobs) + 1)
    data = bytearray()
    offs = []
    for b in blobs:
        offs.append(table + len(data))
        data += b
    bank, addr = out.rom.blob(bytes(table) + bytes(data))
    full = bytearray()
    for o in offs:
        full += (addr + o).to_bytes(2, "little")
    full += b"\0\0"
    full += data
    out.rom.put(bank, addr - 0x8000, bytes(full))
    out.define("SFX_BANK", bank)
    out.define("SFX_TABLE", "((const unsigned int *)0x%04X)" % addr)
    for i, n in enumerate(NAMES):
        out.define("SFX_%s" % n, i + 1)
    out.define("SFX_COUNT", len(NAMES))
    print("sound effects: %d clips, %d bytes" % (len(blobs), len(full)))
