#!/usr/bin/env python3
"""Arrange the original JJ1 (MS-DOS) PSM tracker songs for the Game Gear PSG.

Pipeline:
  1. psm.play() runs the module (4 or 8 channels) tick by tick: sample,
     playback rate, volume and sample position of every channel.
  2. Every sample the song uses is measured (instruments()): its pitch from
     the harmonics of its steady part (smpanalysis.py, two-way mismatch), the
     pitch class confirmed against the song itself (pitchcheck.py: the
     sample's notes mixed from the original data must have the same chroma
     as the arranged pitches), chords (major, minor, power, sus) detected the
     same way, its own pitch movement tracked (bends, tom drops, sweeps), and
     drums told from pitched sounds.  Names only say which drum a drum is.
     Roles follow: bass, lead, accompaniment, toms/effects.
  3. Ticks are laid on the GG's 60 Hz frame grid.  The frame clock restarts at
     every order-list entry, so a pattern that repeats renders to the same
     bytes and is stored once.
  4. Every note becomes a voice with a pitch per frame (slides, vibrato and
     the sample's own movement included; played exactly when it is off the
     semitone grid) and a loudness envelope: the sample's RMS per video frame
     at that playback rate, A-weighted and divided by the A-weighted level of
     the PSG sound playing it, so a kick sample and a square wave of the same
     perceived loudness get the same volume.  Chords add voices for their
     other notes, which only take channels nobody else needs.  A bass is
     moved up by whole octaves into the PSG's range (lowest note A2), single
     notes below that folded up.
  5. Each frame the voices compete for the 3 tone channels (role first, then
     loudness, sticking to the channel they hold) and the noise channel
     (newest hit); the lowest-priority voice ends up on tone 2, which the
     sound effects borrow.
  6. The result is a small event stream (note on with envelope id, level
     change, new note, exact period, note off, wait n frames) that
     src/music.c plays from the frame interrupt.

RefPlayer is the bit-exact Python model of the Z80 player: it yields the PSG
register writes per frame, used for the WAV previews and to check the ROM's
PSG output in the emulator harness."""
import math, os, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psm, smpanalysis

PSG_CLOCK = 3579545
FPS = 60
NOTE_BASE = 45          # note index 0 = A2 (110 Hz), the lowest the PSG reaches
NUM_NOTES = 64

ARP_OFS = {0: (0, 0, 0), 1: (0, 4, 7), 2: (0, 3, 7), 3: (0, 7, 12)}

# noise control values (low 3 bits of the 0xE0 latch)
N_KICK, N_SNARE, N_HAT = 0x02, 0x05, 0x04   # periodic /2048, white /1024, white /512


def sample_key(s):
    return s.name.lower().split(".")[0].strip()


# ------------------------------------------------------ instrument analysis
# Every sample a song uses is measured (tools/smpanalysis.py: pitch by a
# two-way mismatch fit of its harmonics, noisiness, brightness).  The pitch
# class is then checked against the song itself (tools/pitchcheck.py): the
# sample's notes mixed from the original data must have the same chroma as
# square waves at the measured pitch, otherwise the pitch class that matches
# best is taken.  Names only decide what kind of drum a drum is.
KICK_WORDS = ("bass drum", "bassd", "bassdrum", "bd", "kick", "hitbasd", "drum-ben", "gmbd", "funkbd")
SNARE_WORDS = ("snare", "sd", "rimsh", "clap", "snar", "janetsna", "snarev", "ntrdr", "reverssd", "funksd")
HAT_WORDS = ("hat", "hh", "cym", "ride", "shaker", "crash", "ophat", "tamb", "splash", "rangle")
TOM_WORDS = ("tom", "bongo", "conga", "timb")
CHORD_WORDS = {"major": ("maj", "major", "chorma"), "minor": ("min", "minor", "chormi"),
               "power": ("powerch", "metalcho", "lorchit", "orch hit", "orchit", "disthit"),
               "sus2": ("sus 2", "sus2"), "sus4": ("sus4", "sus 4")}
FX_WORDS = ("sweep", "glass", "lazer", "laser", "fx", "weow", "call", "jackr", "1993", "future crew",
            "zine", "reverb", "level ", "brkglass", "blockrev", "hoowa", "if_you")
CHORD_OFS = {None: (0,), "major": (0, 4, 7), "minor": (0, 3, 7), "power": (0, 7),
             "sus2": (0, 2, 7), "sus4": (0, 5, 7)}
# the pieces of a name: "1funkbd" -> has "bd", "SnareRev" -> "snare"
def _has(name, words):
    return any(w in name for w in words)


def a_weight(f):
    """IEC 61672 A-weighting as a linear gain (1.0 at 1 kHz)."""
    f = np.maximum(np.asarray(f, float), 1.0)
    f2 = f * f
    ra = (12194.0 ** 2 * f2 * f2) / ((f2 + 20.6 ** 2) * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2)) *
                                     (f2 + 12194.0 ** 2))
    return ra / 0.7943


def _square_gain(hz):
    k = np.arange(1, 60, 2)
    k = k[k * hz < 20000]
    return math.sqrt(float(np.sum(8.0 / (math.pi ** 2 * k * k) * a_weight(k * hz) ** 2)))


def _noise_gain(ctrl):
    """A-weighted RMS of the PSG noise channel at full volume (unit amplitude)."""
    rate = 44100
    w = synth([[0xE0 | ctrl, 0xF0]] + [[]] * 29, rate)
    sp = np.abs(np.fft.rfft(w)) ** 2
    fr = np.fft.rfftfreq(len(w), 1.0 / rate)
    return math.sqrt(float(np.sum(sp * a_weight(fr) ** 2) / len(w) ** 2 * 2))


class Inst:
    def __repr__(self):
        if self.kind == "noise":
            return "noise %02X" % self.ctrl
        return "tone root %.2f prio %s%s%s" % (self.root, self.prio, (" " + self.chord) if self.chord else "",
                                              " shift %+d" % self.shift if self.shift else "")


def _spec(s):
    d = np.asarray(s.data[:32768], float)
    d = d - d.mean()
    pw = np.abs(np.fft.rfft(d, 1 << 16)) ** 2
    return np.fft.rfftfreq(1 << 16, 1.0 / s.c5), pw


def _refine_octave(s, r0, r):
    """Of r-12, r, r+12 the one whose harmonic series fits the sample best."""
    if r == r0:
        return r0
    fr, sp = smpanalysis._spectrum(s)
    peaks = smpanalysis._peaks(fr, sp) if fr is not None else []
    if not peaks:
        return r
    cands = [c for c in (r - 12, r, r + 12) if 20 <= c <= 110]
    return min(cands, key=lambda c: smpanalysis._twm(peaks, 440.0 * 2 ** ((c - 69) / 12.0)))


def instruments(mod, ticks, verbose=False):
    import pitchcheck
    use = {}
    for secs, st in ticks:
        for c in st:
            if c is not None:
                u = use.setdefault(c[0], [0, [], 0.0])
                u[2] += secs * c[2]
                if c[5]:
                    u[0] += 1
                    u[1].append(c[1])
    insts = {}
    tones = []
    for num, (onsets, freqs, weight) in use.items():
        s = mod.samples.get(num)
        if s is None or len(s.data) <= 16:
            continue
        name = s.name.lower()
        info = smpanalysis.analyse(s)
        it = Inst()
        it.num, it.name, it.chord, it.shift = num, s.name, None, 0
        it.fr, it.pw = _spec(s)
        drum = None
        if _has(name, KICK_WORDS) and not _has(name, ("bass guit", "bass loop", "superhyper", "lead", "bassg")):
            drum = N_KICK
        elif _has(name, HAT_WORDS):
            drum = N_HAT
        elif _has(name, SNARE_WORDS) and "bass" not in name:
            drum = N_SNARE
        tom = _has(name, TOM_WORDS)
        root, best, sim, low = info.root, 0, 0.0, False
        if drum is None and root is not None:
            named = None
            for ck, words in CHORD_WORDS.items():
                if _has(name, words):
                    named = ck
            sims = pitchcheck.check(mod, ticks, num, root, 400)
            if sims:
                # a chord when its chroma fits clearly better than one note
                best_c, best_v = None, max(sims)
                notes = sorted(root + 12 * math.log2(f / s.c5) for f in freqs) or [root]
                low = notes[len(notes) // 2] < 50 or "bass" in name
                for ck, ofs in CHORD_OFS.items():
                    if ck is None or tom or (low and ck != named):
                        continue
                    if named and ck != named:
                        continue
                    cs = pitchcheck.check(mod, ticks, num, root, 400, ofs)
                    margin = 0.0 if ck == named else 0.10
                    if cs and max(cs) > best_v + margin:
                        best_c, best_v, sims = ck, max(cs) - margin, cs
                it.chord = best_c
                best = int(np.argmax(sims))
                if sims[best] < sims[0] + 0.03:
                    best = 0
                sim = sims[best]
                if best > 6:
                    best -= 12
        if drum is None and root is not None and (tom or sim >= 0.70 or (low and sim >= 0.5)):
            it.kind = "tone"
            it.root = _refine_octave(s, root, root + best)
            it.tom = tom
            it.contour = smpanalysis.contour(s, info.root)
            it.onsets = onsets
            it.notes = [it.root + 12 * math.log2(f / s.c5) for f in freqs]
            it.weight = weight
            tones.append(it)
        else:
            it.kind = "noise"
            if drum is None:
                drum = N_HAT if info.bright > 2500 else N_SNARE if info.bright > 350 else N_KICK
            it.ctrl = drum
        it.info, it.sim = info, sim
        insts[num] = it
    # ---- roles: bass, lead, accompaniment
    for it in tones:
        lo = sorted(it.notes)
        med = lo[len(lo) // 2] if lo else it.root
        it.median = med
        name = it.name.lower()
        if it.tom or _has(name, FX_WORDS):
            it.prio = 1
        elif it.chord:
            it.prio = 2
        elif med < 52 or "bass" in name:
            it.prio = 3
        else:
            it.prio = None
    melodic = [it for it in tones if it.prio is None]
    melodic.sort(key=lambda it: -it.weight)
    for i, it in enumerate(melodic):
        it.prio = 4 if i == 0 or it.weight * 2 >= melodic[0].weight else 2
    # ---- register: move a whole instrument by octaves into the PSG's range
    lo_note, hi_note = NOTE_BASE, NOTE_BASE + NUM_NOTES - 1
    for it in tones:
        notes = sorted(it.notes) or [it.root]
        k = 0
        # as few octaves as possible: a few low notes are folded up singly
        # rather than moving a bass line into the melody's register
        while notes[int(len(notes) * 0.5)] + k < lo_note - 0.5 and k < 48:
            k += 12
        while notes[int(len(notes) * 0.9)] + k > hi_note and k > -48:
            k -= 12
        it.shift = k
    if verbose:
        for num, it in sorted(insts.items()):
            print("  %-24s %-40s %s sim %.2f" % (it.name[:24], it, it.info, it.sim))
    return insts


def psg_period(midi):
    return max(1, min(1023, int(round(PSG_CLOCK / (32.0 * 440.0 * 2 ** ((midi - 69) / 12.0))))))


PERIODS = [psg_period(NOTE_BASE + i) for i in range(NUM_NOTES + 12)]


class Envelopes:
    """Per (sample, playback rate, PSG sound) loudness envelopes, frame by frame.

    Loudness is matched perceptually: the sample's RMS per video frame times
    its A-weighting gain at that playback rate, divided by the A-weighted gain
    of the PSG sound that plays it (a square wave at that pitch, or the noise
    mode).  The loudest moment of the song becomes attenuation 0."""

    def __init__(self, mod, insts):
        self.mod, self.insts = mod, insts
        self.db = {}            # key -> (dB per frame, hold)
        self.sq = {}
        for s in mod.samples.values():
            if len(s.data) > 16:
                d = np.array(s.data, float)
                self.sq[s.num] = np.concatenate([[0.0], np.cumsum(d * d)])
        self.ids, self.tables, self.att = {}, [], {}
        self.ref = None

    def rms(self, s, a, b):
        """RMS of sample positions [a, b) with looping."""
        cs = self.sq[s.num]
        n = len(s.data)
        tot, cnt = 0.0, 0
        pos = a
        while pos < b:
            ip = int(pos)
            if s.loop and ip >= s.loop_end:
                span = s.loop_end - s.loop_start
                shift = ((ip - s.loop_start) // span) * span
                pos -= shift; b -= shift; ip = int(pos)
            if ip >= n:
                break
            end = min(int(b), s.loop_end if s.loop else n, n)
            if end <= ip:
                end = ip + 1
            tot += cs[end] - cs[ip]
            cnt += end - ip
            pos = end
        if not cnt:
            return 0.0
        return math.sqrt(tot / cnt)

    def a_gain(self, it, freq):
        s = self.mod.samples[it.num]
        w = a_weight(it.fr * (freq / s.c5)) ** 2
        tot = it.pw.sum() or 1.0
        return math.sqrt(float((it.pw * w).sum() / tot))

    def key(self, smp, freq, psg_gain):
        k = (smp, int(round(freq)), round(psg_gain, 3))
        if k in self.db:
            return k
        s = self.mod.samples[smp]
        it = self.insts[smp]
        g = self.a_gain(it, freq) / psg_gain
        step = freq / FPS
        out, hold = [], False
        pos = 0.0
        for n in range(1200):
            r = self.rms(s, pos, pos + step) * g if smp in self.sq else 0.0
            out.append(20 * math.log10(r) if r > 0 else -200.0)
            pos += step
            if not s.loop and pos >= len(s.data):
                break
            if s.loop and n > 4 and pos > s.loop_end + (s.loop_end - s.loop_start):
                hold = True
                break
        self.db[k] = (out, hold)
        return k

    def peak(self, k):
        return max(self.db[k][0])

    def finish(self, ref):
        """Quantise every envelope against the song's reference level."""
        self.ref = ref

    def get(self, k):
        """(envelope id, attenuation per frame, hold) for an envelope key."""
        if k not in self.att:
            db, hold = self.db[k]
            curve = [max(0, min(15, int(round((self.ref - x) / 2.0)))) for x in db]
            enc = encode_env(curve, hold)
            if enc not in self.ids:
                self.ids[enc] = len(self.tables)
                self.tables.append(enc)
            self.att[k] = (self.ids[enc], curve, hold)
        return self.att[k]


def encode_env(curve, hold):
    """Runs of (att, 1..15 frames); 0x00 = end (silent), 0x10 = hold."""
    out = []
    # trailing silence is implied by END
    while curve and curve[-1] == 15 and not hold:
        curve = curve[:-1]
    i = 0
    while i < len(curve):
        a = curve[i]
        n = 1
        while i + n < len(curve) and curve[i + n] == a and n < 15:
            n += 1
        out.append((a << 4) | n)
        i += n
    out.append(0x10 if hold else 0x00)
    return bytes(out)


def env_expand(enc):
    """Frame-by-frame attenuations of an encoded envelope (for the model)."""
    out = []
    for b in enc:
        if b == 0x00:
            return out, False
        if b == 0x10:
            return out, True
        out += [b >> 4] * (b & 15)
    return out, False


def vol_att(v):
    if v <= 0:
        return 16
    return int(round(-20 * math.log10(v / 64.0) / 2))


class Voice:
    """One note of one tracker channel, as the PSG should play it."""


SQUARE_GAINS = {}
NOISE_GAINS = {}


def square_gain(midi):
    n = int(round(midi))
    if n not in SQUARE_GAINS:
        SQUARE_GAINS[n] = _square_gain(440.0 * 2 ** ((n - 69) / 12.0))
    return SQUARE_GAINS[n]


def noise_gain(ctrl):
    if ctrl not in NOISE_GAINS:
        NOISE_GAINS[ctrl] = _noise_gain(ctrl)
    return NOISE_GAINS[ctrl]


def build(path, loops=1, verbose=False):
    mod = psm.Module(path)
    ticks, rowmarks = psm.play(mod, loops=loops)
    mod.insts = instruments(mod, ticks, verbose)
    envs = Envelopes(mod, mod.insts)
    # ---- frame grid: restart the frame clock at each order entry
    seg_bounds = []          # (first tick, order index)
    for t, oi, ri in rowmarks:
        if not seg_bounds or seg_bounds[-1][1] != oi:
            seg_bounds.append((t, oi))
    seg_bounds.append((len(ticks), None))
    tick_frame = [0] * len(ticks)
    segments = []            # (start frame, length, order index)
    frame = 0
    for si in range(len(seg_bounds) - 1):
        t0, oi = seg_bounds[si]
        t1 = seg_bounds[si + 1][0]
        acc = 0.0
        for t in range(t0, t1):
            tick_frame[t] = frame + int(round(acc * FPS))
            acc += ticks[t][0]
        n = int(round(acc * FPS))
        segments.append((frame, n, oi))
        frame += n
    total = frame
    # ---- per channel, per frame state
    nch = mod.channels
    voices = []

    def close(v, k):
        v.end = k
        voices.append(v)
        for g in v.ghosts:
            g.end = k
            voices.append(g)
    for ch in range(nch):
        cur = None
        per_frame = [None] * total
        trig = [False] * total
        for t, (secs, st) in enumerate(ticks):
            f = tick_frame[t]
            nxt = tick_frame[t + 1] if t + 1 < len(ticks) else total
            s = st[ch]
            for k in range(f, max(f + 1, nxt)):
                if k < total:
                    per_frame[k] = s
            if s is not None and s[5] and f < total:
                trig[f] = True
        for k in range(total):
            s = per_frame[k]
            if s is None:
                if cur:
                    close(cur, k); cur = None
                continue
            smp, freq, vol, pos, note, _ = s
            if trig[k] or cur is None or cur.onset != smp:
                if cur:
                    close(cur, k)
                cur = new_voice(mod, envs, ch, k, smp, freq, note)
                if cur is None:
                    continue
            if cur is None:
                continue
            cur.base.append(vol_att(vol))
            cur.period.append(freq)
            cur.pos.append(pos)
        if cur:
            close(cur, total)
    # ---- the song's reference level: its loudest voice plays at attenuation 0
    ref = max(envs.peak(v.ekey) - 2.0 * min(v.base) for v in voices if v.base)
    envs.finish(ref)
    for v in voices:
        v.env, v.curve, v.hold = envs.get(v.ekey)
    return mod, envs, voices, segments, total


def new_voice(mod, envs, ch, start, smp, freq, note):
    s = mod.samples.get(smp)
    it = mod.insts.get(smp)
    if s is None or it is None or note is None:
        return None
    v = Voice()
    v.ch, v.start, v.onset, v.it = ch, start, smp, it
    v.base, v.period, v.pos, v.ghosts = [], [], [], []
    v.arp = 0
    if it.kind == "tone":
        v.kind, v.prio, v.noise, v.ofs = "tone", it.prio, 0, 0
        pitch = it.root + 12 * math.log2(freq / s.c5) + it.shift
        v.ekey = envs.key(smp, freq, square_gain(pitch))
        for o in CHORD_OFS[it.chord][1:]:
            g = Voice()
            g.__dict__.update(v.__dict__)
            g.ghosts, g.ofs, g.prio = [], o, 1.5
            v.ghosts.append(g)
    else:
        v.kind, v.prio, v.noise, v.ofs = "noise", 0, it.ctrl, 0
        v.ekey = envs.key(smp, freq, noise_gain(it.ctrl))
    return v


# ------------------------------------------------------------- arrangement
EV_WAIT, EV_NOTE, EV_NOTEOFS, EV_OFF, EV_BASE, EV_SETNOTE, EV_SETPER, EV_PERDELTA = range(8)
CENTS = 0.06   # exact-pitch changes smaller than this (semitones) are not sent
FINE = 0.12      # semitones a pitch may be off its note before it is played exactly


def song_detune(mod, voices):
    """The song's overall tuning offset (in semitones, -0.5..0.5)."""
    x = y = 0.0
    for v in voices:
        if v.kind == "tone" and v.period and not v.ofs:
            s = mod.samples[v.onset]
            p = v.it.root + 12 * math.log2(v.period[0] / s.c5)
            a = 2 * math.pi * p
            x += math.cos(a); y += math.sin(a)
    return math.atan2(y, x) / (2 * math.pi) if (x or y) else 0.0


def tone_target(v, freq_hz, smp_c5, detune, pos=0.0):
    """('n', note index) when the pitch is on a semitone, else ('p', period)."""
    midi = v.it.root + 12 * math.log2(freq_hz / smp_c5) + v.it.shift + v.ofs - detune
    if v.it.contour is not None:
        midi += float(np.interp(pos, v.it.contour[0], v.it.contour[1]))
    n = int(round(midi)) - NOTE_BASE
    k = 0
    while n + k < 0:
        k += 12
    while n + k >= NUM_NOTES:
        k -= 12
    n += k
    if abs(midi - round(midi)) <= FINE:
        return ("n", n)
    return ("p", psg_period(midi + k))


def arrange(mod, voices, total):
    """Per frame, give voices to PSG channels; return events per frame."""
    detune = song_detune(mod, voices)
    starts = {}
    for v in voices:
        starts.setdefault(v.start, []).append(v)
        c5 = mod.samples[v.onset].c5
        if v.kind == "tone":
            v.targets = [tone_target(v, f, c5, detune, p) for f, p in zip(v.period, v.pos)]
            t0 = v.targets[0]
            v.n0 = t0[1] if t0[0] == "n" else int(round(v.it.root + 12 * math.log2(v.period[0] / c5) +
                                                       v.it.shift + v.ofs - detune)) - NOTE_BASE
            v.n0 = max(0, min(NUM_NOTES - 1, v.n0))
        v.sounding = [(v.curve[k] if k < len(v.curve) else (v.curve[-1] if v.hold and v.curve else 15))
                      for k in range(v.end - v.start)]
    active = []
    holder = [None, None, None, None]      # voice on tone0..2, noise
    shown = {}                              # (channel, voice) -> (base, target) last sent
    events = [[] for _ in range(total)]
    for f in range(total):
        active += starts.get(f, [])
        active = [v for v in active if v.end > f]

        def level(v):
            k = f - v.start
            return v.sounding[k] + v.base[k]
        tones = [v for v in active if v.kind == "tone" and level(v) < 15]
        noises = [v for v in active if v.kind == "noise" and level(v) < 15]
        # ---- noise: newest hit wins, then loudest
        want_noise = None
        if noises:
            want_noise = max(noises, key=lambda v: (v.start, -level(v)))
        # ---- tones: the role counts 10 dB, loudness the rest; a voice that
        # already holds a channel keeps it unless clearly beaten
        def score(v):
            return v.prio * 5 - level(v) + (1.5 if v in holder[:3] else 0)
        tones.sort(key=lambda v: (-score(v), v.start))
        chosen = tones[:3]
        new_hold = [None, None, None]
        for i in range(3):
            if holder[i] in chosen:
                new_hold[i] = holder[i]
        rest = [v for v in chosen if v not in new_hold]
        # high priority to the lowest free channel (tone 2 is shared with SFX)
        for v in sorted(rest, key=lambda v: -v.prio):
            free = [i for i in range(3) if new_hold[i] is None]
            new_hold[free[0]] = v
        ev = events[f]
        for i in range(4):
            v = want_noise if i == 3 else new_hold[i]
            old = holder[i]
            if v is None:
                if old is not None:
                    ev.append((EV_OFF, i))
                holder[i] = None
                continue
            k = f - v.start
            base = min(15, v.base[k])
            tgt = v.targets[k] if v.kind == "tone" else ("n", v.noise)
            if v is not old:
                note = v.noise if v.kind == "noise" else (tgt[1] if tgt[0] == "n" else v.n0)
                ev.append((EV_NOTEOFS if k else EV_NOTE, i, note, v.env, base, k))
                if tgt[0] == "p":
                    ev.append((EV_SETPER, i, tgt[1]))
                shown[(i, id(v))] = (base, tgt)
                holder[i] = v
                continue
            pb, pt = shown[(i, id(v))]
            if base != pb:
                ev.append((EV_BASE, i, base))
            if tgt != pt:
                if tgt[0] == "n":
                    ev.append((EV_SETNOTE, i, tgt[1]))
                elif pt[0] == "p" and abs(12 * math.log2(pt[1] / tgt[1])) < CENTS:
                    tgt = pt                      # too small to hear: keep the old period
                elif pt[0] == "p" and -128 <= tgt[1] - pt[1] < 128:
                    ev.append((EV_PERDELTA, i, tgt[1] - pt[1]))
                else:
                    ev.append((EV_SETPER, i, tgt[1]))
            shown[(i, id(v))] = (base, tgt)
    # silence held notes at the loop point
    for i in range(4):
        if holder[i] is not None:
            events[total - 1].append((EV_OFF, i))
    return events


def encode(events, segments, total):
    """Event bytes per order segment; identical segments are stored once."""
    segs = []
    for (f0, n, oi) in segments:
        out = bytearray()
        wait = 0
        for f in range(f0, f0 + n):
            ev = events[f]
            if ev and wait:
                while wait > 63:
                    out.append(63); wait -= 63
                out.append(wait); wait = 0
            for e in ev:
                if e[0] == EV_NOTE:
                    out += bytes([0x40 | e[1], e[2], e[3], e[4]])
                elif e[0] == EV_NOTEOFS:
                    out += bytes([0x44 | e[1], e[2], e[3], e[4], min(255, e[5])])
                elif e[0] == EV_OFF:
                    out.append(0x48 | e[1])
                elif e[0] == EV_BASE:
                    out += bytes([0x4C | e[1], e[2]])
                elif e[0] == EV_SETNOTE:
                    out += bytes([0x50 | e[1], e[2]])
                elif e[0] == EV_SETPER:
                    out += bytes([0x54 | e[1], e[2] & 0xFF, e[2] >> 8])
                elif e[0] == EV_PERDELTA:
                    out += bytes([0x58 | e[1], e[2] & 0xFF])
            wait += 1
        while wait > 63:
            out.append(63); wait -= 63
        if wait:
            out.append(wait)
        out.append(0)
        segs.append(bytes(out))
    return segs


_PARTS = {}


def song_parts(path, loop=True):
    """The arranged song: unique segments, order (segment indices), restart
    order, envelope tables and statistics.  Cached per (path, loop)."""
    key = (os.path.abspath(path), loop)
    if key not in _PARTS:
        mod, envs, voices, segments, total = build(path)
        events = arrange(mod, voices, total)
        segs = encode(events, segments, total)
        uniq, order = [], []
        for sg in segs:
            if sg not in uniq:
                uniq.append(sg)
            order.append(uniq.index(sg))
        restart = 0
        for i, (f0, n, oi) in enumerate(segments):
            if oi == mod.restart:
                restart = i
                break
        info = dict(frames=total, orders=len(order), unique=len(uniq), envs=len(envs.tables),
                    size=sum(map(len, uniq)) + 4 + 3 * len(order) + sum(2 + len(t) for t in envs.tables),
                    voices=len(voices))
        _PARTS[key] = dict(segs=uniq, order=order, restart=restart if loop else 0xFF,
                           envs=envs.tables, info=info)
    return _PARTS[key]


def home_blob(parts, seg_locs, base_addr):
    """The song's home block (it holds the envelopes, so the player keeps its
    bank mapped while it steps the channels):
      [orders][restart, 0xFF = play once][envelope table ptr]
      [order -> segment address, segment bank]... [envelope ptrs]... envelopes.
    seg_locs[i] = (bank, address) of unique segment i."""
    order, envs = parts["order"], parts["envs"]
    etab = 4 + 3 * len(order)
    pos = etab + 2 * len(envs)
    env_addr = []
    for t in envs:
        env_addr.append(base_addr + pos); pos += len(t)
    blob = bytearray([len(order), parts["restart"]]) + struct.pack("<H", base_addr + etab)
    for o in order:
        bank, addr = seg_locs[o]
        blob += struct.pack("<HB", addr, bank)
    for a in env_addr:
        blob += struct.pack("<H", a)
    for t in envs:
        blob += t
    assert len(blob) == pos
    return bytes(blob)


def song_image(path, loop=True):
    """A stand-alone ROM image of one song for the tools and tests: home
    block at bank 1, 0x8000, segments first-fit from bank 2.
    Returns (image, home bank, home address, info)."""
    parts = song_parts(path, loop)
    banks = [bytearray(), bytearray()]
    locs = []
    for sg in parts["segs"]:
        for i in range(2, len(banks)):
            if len(banks[i]) + len(sg) <= 0x4000:
                break
        else:
            banks.append(bytearray()); i = len(banks) - 1
        locs.append((i, 0x8000 + len(banks[i])))
        banks[i] += sg
    banks[1] = bytearray(home_blob(parts, locs, 0x8000))
    img = b"".join(bytes(b) + bytes(0x4000 - len(b)) for b in banks)
    return img, 1, 0x8000, parts["info"]


# ------------------------------------------------- reference player (model)
class RefPlayer:
    """Bit-exact model of src/music.c.  frame() returns the PSG bytes written.
    rom is a ROM image (16 KB banks); the song's home block is at bank:addr
    (slot-2 addresses)."""

    def __init__(self, rom, bank, addr):
        self.rom, self.home = rom, bank
        self.hdr = addr
        self.norders, self.restart = self.rd(bank, addr), self.rd(bank, addr + 1)
        self.etab = self.rd16(bank, addr + 2)
        self.order = 0
        self.stopped = False
        self.seg(0)
        self.wait = 0
        self.ch = [dict(on=False, note=0, arp=0, phase=0, base=0, env=0, run=0, att=15, hold=False,
                        last_att=15, last_per=0xFFFF, fix=False, per=0) for _ in range(4)]

    def rd(self, bank, addr):
        return self.rom[bank * 0x4000 + addr - 0x8000]

    def rd16(self, bank, addr):
        return self.rd(bank, addr) | self.rd(bank, addr + 1) << 8

    def seg(self, i):
        e = self.hdr + 4 + 3 * i
        self.ptr = self.rd16(self.home, e)
        self.sbank = self.rd(self.home, e + 2)

    def env_ptr(self, i):
        return self.rd16(self.home, self.etab + 2 * i)

    def step_env(self, c):
        if c["hold"]:
            return
        if c["run"] == 0:
            e = self.rd(self.home, c["env"])
            if e == 0x00:
                c["on"] = False
                return
            if e == 0x10:
                c["hold"] = True
                return
            c["env"] += 1
            c["att"] = e >> 4
            c["run"] = e & 15
        c["run"] -= 1

    def release(self, i):
        """A sound effect gave channel i back: rewrite its registers."""
        self.ch[i]["last_att"] = 0xFF
        self.ch[i]["last_per"] = 0xFFFF

    def frame(self, sfx_mask=0):
        if self.stopped:
            return []
        if self.wait:
            self.wait -= 1
        else:
            sb = lambda k: self.rd(self.sbank, self.ptr + k)
            while True:
                op = sb(0)
                self.ptr += 1
                if op == 0:
                    self.order += 1
                    if self.order >= self.norders:
                        if self.restart == 0xFF:          # jingle: stop
                            self.stopped = True
                            out = []
                            for i, c in enumerate(self.ch):
                                c["on"] = False
                                if not sfx_mask & (1 << i) and c["last_att"] != 15:
                                    out.append(0x90 | (i << 5) | 15)
                                    c["last_att"] = 15
                            return out
                        self.order = self.restart
                    self.seg(self.order)
                    continue
                if op < 0x40:
                    self.wait = op - 1
                    break
                i = op & 3
                c = self.ch[i]
                kind = op & 0xFC
                if kind in (0x40, 0x44):
                    c["note"] = sb(0)
                    c["env"] = self.env_ptr(sb(1))
                    c["base"] = sb(2) & 15
                    c["arp"] = sb(2) >> 4
                    self.ptr += 3
                    c["on"], c["run"], c["hold"], c["phase"], c["att"] = True, 0, False, 0, 15
                    c["fix"] = False
                    if kind == 0x44:
                        ofs = sb(0)
                        self.ptr += 1
                        while ofs and c["on"] and not c["hold"]:
                            self.step_env(c)
                            ofs -= 1
                    if i == 3:
                        c["last_per"] = 0xFFFF      # rewrite the noise mode
                elif kind == 0x48:
                    c["on"] = False
                elif kind == 0x4C:
                    c["base"] = sb(0) & 15
                    self.ptr += 1
                elif kind == 0x50:
                    c["note"] = sb(0)
                    c["fix"] = False
                    self.ptr += 1
                elif kind == 0x54:
                    c["per"] = sb(0) | sb(1) << 8
                    c["fix"] = True
                    self.ptr += 2
                elif kind == 0x58:
                    d = sb(0)
                    c["per"] = (c["per"] + (d - 256 if d >= 128 else d)) & 0xFFFF
                    self.ptr += 1
        out = []
        for i, c in enumerate(self.ch):
            if c["on"]:
                self.step_env(c)
            att = min(15, c["att"] + c["base"]) if c["on"] else 15
            if i < 3:
                n = c["note"] + ARP_OFS[c["arp"]][c["phase"]]
                c["phase"] = c["phase"] + 1 if c["phase"] < 2 else 0
                per = c["per"] if c["fix"] else PERIODS[n]
            else:
                per = c["note"]
            if sfx_mask & (1 << i):
                continue
            if c["on"] and per != c["last_per"]:
                if i < 3:
                    out += [0x80 | (i << 5) | (per & 15), (per >> 4) & 0x3F]
                else:
                    out.append(0xE0 | per)
                c["last_per"] = per
            if att != c["last_att"]:
                out.append(0x90 | (i << 5) | att)
                c["last_att"] = att
        return out


def synth(frames_writes, rate=44100):
    """Render PSG register writes (one list per frame) to float samples."""
    per = [1, 1, 1]
    att = [15, 15, 15, 15]
    nctl, latch = 0, 0
    vol_tab = [10 ** (-0.1 * a) if a < 15 else 0.0 for a in range(16)]
    phase = [0.0, 0.0, 0.0]
    lfsr, ncnt = 0x8000, 0.0
    spf = rate // FPS
    clk = PSG_CLOCK / 16.0
    out = np.zeros(len(frames_writes) * spf)
    t = np.arange(spf)
    for fi, w in enumerate(frames_writes):
        for byte in w:
            if byte & 0x80:
                latch = (byte >> 4) & 7
                ch, typ = latch >> 1, latch & 1
                if typ:
                    att[ch] = byte & 15
                elif ch < 3:
                    per[ch] = (per[ch] & 0x3F0) | (byte & 15)
                else:
                    nctl = byte & 7
                    lfsr = 0x8000
            else:
                ch = latch >> 1
                if ch < 3 and not (latch & 1):
                    per[ch] = (per[ch] & 15) | ((byte & 0x3F) << 4)
        s = np.zeros(spf)
        for c in range(3):
            if att[c] < 15 and per[c] > 1:
                inc = clk / (2.0 * per[c]) / rate          # half-periods per sample
                ph = phase[c] + inc * t
                s += np.where((np.floor(ph) % 2) == 0, 1.0, -1.0) * vol_tab[att[c]]
                phase[c] = (phase[c] + inc * spf) % 2.0
        if att[3] < 15:
            r = nctl & 3
            np_ = (0x10, 0x20, 0x40)[r] if r < 3 else max(1, per[2])
            shifts_per_sample = clk / np_ / rate
            pos = ncnt + shifts_per_sample * t
            end = ncnt + shifts_per_sample * spf
            done = int(end)
            bits = np.empty(done + 1)
            st = lfsr
            for k in range(done + 1):
                bits[k] = st & 1
                if k == done:
                    break
                fb = ((st & 1) ^ ((st >> 3) & 1)) if nctl & 4 else (st & 1)
                st = (st >> 1) | (fb << 15)
            lfsr = st
            ncnt = end - done
            s += (bits[np.floor(pos).astype(int)] * 2 - 1) * vol_tab[att[3]]
        out[fi * spf:(fi + 1) * spf] = s
    return out


def write_wav(buf, path, rate=44100):
    import wave
    buf = buf - np.convolve(buf, np.ones(256) / 256, "same")
    pk = np.abs(buf).max() or 1
    data = (buf / pk * 0.8 * 32767).astype("<i2")
    wf = wave.open(path, "wb")
    wf.setnchannels(1); wf.setsampwidth(2); wf.setframerate(rate)
    wf.writeframes(data.tobytes())
    wf.close()


def reference_writes(rom, bank, addr, frames):
    p = RefPlayer(rom, bank, addr)
    return [p.frame() for _ in range(frames)]


if __name__ == "__main__":
    for p in sys.argv[1:]:
        print(os.path.basename(p), song_parts(p)["info"])
