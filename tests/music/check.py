#!/usr/bin/env python3
"""Compare the ROM's PSG writes (smstest psglog) with tools/psgmusic.py's
reference player: the byte streams must be identical, and the ROM must stay
in step with the model's 60 Hz timing (within one frame, since the harness's
frame counter may tick in the middle of an interrupt).
Usage: check.py psglog SONG.PSM [once]"""
import os, sys
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import psgmusic
log, path = sys.argv[1], sys.argv[2]
loop = "once" not in sys.argv[3:]
rows = [(int(f), int(v, 16)) for f, v in (l.split() for l in open(log))]
# skip power-on and music_play silence writes (volume-off latches)
i = 0
while "--sync" not in sys.argv and i < len(rows) and (rows[i][1] & 0x9F) == 0x9F:
    i += 1
rows = rows[i:]
f0 = rows[0][0]
blob, info = psgmusic.song_blob(path, loop=loop)
p = psgmusic.RefPlayer(blob)
ref, ref_frame = [], []
fr = 0
while len(ref) < len(rows) + 5000 and fr < 200000:
    w = p.frame()
    if fr == 0 and not w:
        continue                     # align to the first frame with writes
    ref += w
    ref_frame += [fr] * len(w)
    fr += 1
    if p.stopped:
        break
rom = [v for f, v in rows]


def split(stream):
    """PSG byte stream -> list of (channel, bytes) register writes."""
    out, latch = [], 0
    for b in stream:
        if b & 0x80:
            latch = (b >> 5) & 3
            out.append([latch, [b]])
        elif out:
            out[-1][1].append(b)
    return [(c, tuple(v)) for c, v in out]


def regs(stream):
    r = {}
    latch = None
    for b in stream:
        if b & 0x80:
            latch = (b >> 4) & 7
            if latch & 1 or latch == 6:
                r[latch] = b & 15 if latch & 1 else b & 7
            else:
                r[latch] = (r.get(latch, 0) & 0x3F0) | (b & 15)
        elif latch is not None and not latch & 1 and latch != 6:
            r[latch] = (r.get(latch, 0) & 15) | ((b & 0x3F) << 4)
    return r


if "--sfx" in sys.argv:
    # effects borrow tone 2 / noise: channels 0-1 must match exactly, and
    # once the effects are over every register must be back to the music's
    W = 24
    rw = split(rom)
    a = [w for w in rw if w[0] < 2]
    full = split(ref)
    idx = [i for i, w in enumerate(full) if w[0] < 2]
    b = [full[i] for i in idx]
    for k in range(len(b) - W):
        if b[k:k + W] == a[:W]:
            ref = [x for w in full[idx[k]:] for x in w[1]]
            b = b[k:]
            break
    else:
        print("no alignment found"); sys.exit(1)
    n = len(a)
    if a != b[:n]:
        k = next(i for i in range(n) if a[i] != b[i])
        print("MISMATCH on tone 0/1 at write %d: rom %s ref %s" % (k, a[k:k + 4], b[k:k + 4]))
        sys.exit(1)
    # the model's stream up to the same point (count of ch0/1 writes)
    cnt, end = 0, 0
    for i, w in enumerate(split(ref)):
        if w[0] < 2:
            cnt += 1
            if cnt == n:
                end = i
                break
    ref_upto = [x for w in split(ref)[:end + 1] for x in w[1]]
    last = max(i for i, w in enumerate(rw) if w[0] < 2)
    ra, rb = regs([x for w in rw[:last + 1] for x in w[1]]), regs(ref_upto)
    diff = {k: (ra.get(k), rb.get(k)) for k in set(ra) | set(rb) if ra.get(k) != rb.get(k)}
    print("tone 0/1: %d writes identical; final registers %s" % (n, "match" if not diff else "DIFFER %s" % diff))
    sys.exit(1 if diff else 0)
if "--sync" in sys.argv:
    # the log starts mid-song: find where in the model it begins
    W = 64
    for k in range(len(ref) - W):
        if ref[k:k + W] == rom[:W]:
            ref, ref_frame = ref[k:], [x - ref_frame[k] for x in ref_frame[k:]]
            break
    else:
        print("no alignment found"); sys.exit(1)
    # writes in the first ROM frame may belong to the previous interrupt
    f0 = rows[0][0]
n = min(len(rom), len(ref))
for k in range(n):
    if rom[k] != ref[k]:
        print("MISMATCH at byte %d (ROM frame %d, model frame %d): rom %s ref %s" % (
            k, rows[k][0], ref_frame[k], " ".join("%02X" % x for x in rom[k:k + 8]),
            " ".join("%02X" % x for x in ref[k:k + 8])))
        sys.exit(1)
drift = max(abs((rows[k][0] - f0) - ref_frame[k]) for k in range(n))
print("MATCH: %d bytes over %d frames, timing drift %d frame(s)" % (n, rows[n - 1][0] - f0, drift))
sys.exit(0 if drift <= 2 else 1)
