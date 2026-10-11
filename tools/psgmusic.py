#!/usr/bin/env python3
"""Arrange the original JJ1 (MS-DOS) PSM tracker songs for the Game Gear PSG.

Pipeline:
  1. psm.play() runs the 4-channel module tick by tick (pitch, volume, sample
     position of every channel).
  2. Ticks are laid on the GG's 60 Hz frame grid.  The frame clock restarts at
     every order-list entry, so a pattern that repeats renders to the same
     bytes and is stored once.
  3. Every note becomes a "voice": a role from the instrument table below
     (tone / chord arpeggio / drum on the noise channel), a pitch, and a
     volume envelope measured from the sample itself (RMS per video frame at
     the note's playback rate), so decays, swells and reversed hi-hats keep
     their shape.  Volume column / slides shift the envelope's base level.
  4. Each frame the voices compete for the 3 tone channels and the noise
     channel by priority (lead > bass > chords > toms), sticking to the
     channel they already hold; the lowest-priority voice is kept on tone 2,
     which the sound effects borrow.
  5. The result is a small event stream (note on with envelope id, base
     level change, pitch slide, note off, wait n frames) that src/music.c
     plays from the vertical blank interrupt.

play_reference() is the bit-exact Python model of the Z80 player: it yields
the PSG register writes per frame, used for the WAV previews and to check the
ROM's PSG output in the emulator harness."""
import math, os, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psm

PSG_CLOCK = 3579545
FPS = 60
NOTE_BASE = 45          # note index 0 = A2 (110 Hz), the lowest the PSG reaches
NUM_NOTES = 64

ARP = {None: 0, "major": 1, "minor": 2, "power": 3}
ARP_OFS = {0: (0, 0, 0), 1: (0, 4, 7), 2: (0, 3, 7), 3: (0, 7, 12)}

# noise control values (low 3 bits of the 0xE0 latch)
N_KICK, N_SNARE, N_HAT = 0x02, 0x05, 0x04   # periodic /2048, white /1024, white /512

# Instrument roles by sample name (all pitched JJ1 samples are rooted on C;
# `root` is that C as a MIDI note, measured with an autocorrelation pitch
# detector).  prio: lead 4, bass 3, chords 2, toms/effects 1.
#   ("tone", root, prio, arp, octave_shift)  /  ("noise", ctrl, gain)
ROLES = {
    # Diamondus (SONG0)
    "shaka3":   ("tone", 60, 4, None, 0),
    "monsterb": ("tone", 36, 3, None, 12),
    "majorwec": ("tone", 36, 2, "major", 12),
    "minorwec": ("tone", 48, 2, "minor", 0),
    "simmstom": ("tone", 48, 1, None, 12),
    "bassdrum": ("noise", N_KICK, 0),
    "janetsna": ("noise", N_SNARE, 0),
    "hihatrev": ("noise", N_HAT, 1),
    # Tubelectric (SONG3)
    "luvhorn":  ("tone", 72, 4, None, 0),
    "ieee":     ("tone", 48, 3, None, 12),
    "basscrus": ("tone", 36, 3, None, 12),
    "bddrm":    ("noise", N_KICK, 0),
    "shaker":   ("noise", N_HAT, 2),
    "snarever": ("noise", N_SNARE, 0),
    "realcymc": ("noise", N_HAT, 1),
    "stopoff":  ("noise", N_SNARE, 1),
    # Medivo (SONG17)
    "stern2":   ("tone", 24, 2, None, 24),
    "bassd2":   ("noise", N_KICK, 0),
    "snarev2":  ("noise", N_SNARE, 0),
    "opcchh":   ("noise", N_HAT, 1),
    "clcchh":   ("noise", N_HAT, 1),
    "ohme":     ("tone", 55, 2, None, 0),
    "synb3":    ("tone", 24, 3, None, 24),
    "chormi":   ("tone", 36, 2, "minor", 12),
    "chorma":   ("tone", 36, 2, "major", 12),
    "lorchit2": ("tone", 48, 2, "power", 0),
    "beep":     ("tone", 60, 4, None, 0),
    # title (MENUSNG)
    "gmbdwhh":  ("noise", N_KICK, 0),
    "gmsnare":  ("noise", N_SNARE, 0),
    "fsyntbas": ("tone", 24, 3, None, 24),
    "trumpoct": ("tone", 36, 4, None, 12),
    "dryfctom": ("tone", 36, 1, None, 12),
    "vsride":   ("noise", N_HAT, 1),
    # boss (BOSS)
    "1funkbd":  ("noise", N_KICK, 0),
    "1funksd":  ("noise", N_SNARE, 0),
    "revtom2":  ("tone", 48, 1, None, 0),
    "ridecymb": ("noise", N_HAT, 1),
    "metalcho": ("tone", 36, 3, "power", 12),
    "hoowa":    ("noise", N_SNARE, 2),
    "ringuit":  ("tone", 36, 4, None, 12),
    # level-complete jingle (ENDLEVEL)
    "disthit":  ("tone", 84, 4, None, -12),
    "fltropen": ("tone", 48, 2, None, 12),
}


def sample_key(s):
    return s.name.lower().split(".")[0].strip()


def psg_period(midi):
    return max(1, min(1023, int(round(PSG_CLOCK / (32.0 * 440.0 * 2 ** ((midi - 69) / 12.0))))))


PERIODS = [psg_period(NOTE_BASE + i) for i in range(NUM_NOTES + 12)]


class Envelopes:
    """Per (sample, playback rate) attenuation envelopes, frame by frame."""

    def __init__(self, mod):
        self.mod = mod
        self.cache = {}
        self.ids = {}
        self.tables = []
        self.sq = {}
        ref = 1.0
        for s in mod.samples.values():
            if len(s.data) > 64:
                d = np.array(s.data, float)
                self.sq[s.num] = np.concatenate([[0.0], np.cumsum(d * d)])
                w = max(32, int(s.c5 / FPS))
                k = np.convolve(d * d, np.ones(w) / w, "valid")
                ref = max(ref, math.sqrt(k.max()))
        self.ref = ref * 0.9

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

    def att_curve(self, s, freq, gain):
        """Attenuation (0..15) per frame for a note at `freq` samples/s."""
        step = freq / FPS
        out = []
        pos = 0.0
        for k in range(1200):
            r = self.rms(s, pos, pos + step) if s.num in self.sq else 0.0
            a = r / self.ref
            att = 15 if a <= 0 else int(round(-20 * math.log10(a) / 2)) + gain
            att = max(0, min(15, att))
            out.append(att)
            pos += step
            if not s.loop and pos >= len(s.data):
                break
            if s.loop and k > 4 and pos > s.loop_end + (s.loop_end - s.loop_start):
                return out, True          # sustained: hold the last level
        return out, False

    def get(self, smp, freq, gain):
        key = (smp, int(round(freq)), gain)
        if key not in self.cache:
            s = self.mod.samples[smp]
            curve, hold = self.att_curve(s, freq, gain)
            enc = encode_env(curve, hold)
            if enc not in self.ids:
                self.ids[enc] = len(self.tables)
                self.tables.append(enc)
            self.cache[key] = (self.ids[enc], curve, hold)
        return self.cache[key]


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


def build(path, loops=1):
    mod = psm.Module(path)
    ticks, rowmarks = psm.play(mod, loops=loops)
    envs = Envelopes(mod)
    # ---- frame grid: restart the frame clock at each order entry
    order_of_tick = [0] * len(ticks)
    starts = {}
    for t, oi, ri in rowmarks:
        starts.setdefault((t, oi), True)
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
                    cur.end = k; voices.append(cur); cur = None
                continue
            smp, freq, vol, pos, note, _ = s
            if trig[k] or cur is None or cur.onset != smp:
                if cur:
                    cur.end = k; voices.append(cur)
                cur = new_voice(mod, envs, ch, k, smp, freq, note)
                if cur is None:
                    continue
            if cur is None:
                continue
            cur.base.append(vol_att(vol))
            cur.period.append(freq)
        if cur:
            cur.end = total; voices.append(cur)
    voices = [v for v in voices if v is not None]
    return mod, envs, voices, segments, total


def new_voice(mod, envs, ch, start, smp, freq, note):
    s = mod.samples.get(smp)
    if s is None:
        return None
    role = ROLES.get(sample_key(s))
    if role is None or note is None:
        return None
    v = Voice()
    v.ch, v.start, v.onset = ch, start, smp
    v.base, v.period = [], []
    if role[0] == "tone":
        _, root, prio, arp, shift = role
        v.kind, v.prio, v.arp, v.noise = "tone", prio, ARP[arp], 0
        gain = 0
    else:
        _, ctrl, gain = role
        v.kind, v.prio, v.arp, v.noise = "noise", 0, 0, ctrl
        root, shift = 60, 0
    v.env, v.curve, v.hold = envs.get(smp, freq, gain)
    v.root, v.shift = root, shift
    return v


def tone_note(v, freq_hz, smp_c5):
    """Note index (0 = A2) for a tracker playback frequency."""
    midi = v.root + 12 * math.log2(freq_hz / smp_c5) + v.shift
    n = int(round(midi)) - NOTE_BASE
    while n < 0:
        n += 12
    while n + (12 if v.arp == 3 else 7 if v.arp else 0) >= NUM_NOTES + 12:
        n -= 12
    return n


# ------------------------------------------------------------- arrangement
EV_WAIT, EV_NOTE, EV_NOTEOFS, EV_OFF, EV_BASE, EV_SETNOTE = range(6)


def arrange(mod, voices, total):
    """Per frame, give voices to PSG channels; return events per frame."""
    starts = {}
    for v in voices:
        starts.setdefault(v.start, []).append(v)
        c5 = mod.samples[v.onset].c5
        if v.kind == "tone":
            v.notes = [tone_note(v, f, c5) for f in v.period]
            v.n0 = v.notes[0]
        v.sounding = [ (v.curve[k] if k < len(v.curve) else (v.curve[-1] if v.hold and v.curve else 15))
                       for k in range(v.end - v.start) ]
    active = []
    holder = [None, None, None, None]      # voice on tone0..2, noise
    shown = {}                              # voice -> (base, note) last sent
    events = [[] for _ in range(total)]
    for f in range(total):
        active += starts.get(f, [])
        active = [v for v in active if v.end > f]

        def audible(v):
            k = f - v.start
            return v.sounding[k] + v.base[k] < 15
        tones = [v for v in active if v.kind == "tone" and audible(v)]
        noises = [v for v in active if v.kind == "noise" and audible(v)]
        # ---- noise: newest hit wins, then loudest
        want_noise = None
        if noises:
            want_noise = max(noises, key=lambda v: (v.start, -v.base[f - v.start]))
        # ---- tones: top 3 by priority, then loudness, then age
        tones.sort(key=lambda v: (-v.prio, v.sounding[f - v.start] + v.base[f - v.start], v.start))
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
        # keep the lowest-priority voice on tone 2 when tone 2 changes hands anyway
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
            if v is not old:
                note = v.noise if v.kind == "noise" else v.notes[k]
                ev.append((EV_NOTEOFS if k else EV_NOTE, i, note, v.env, base | (v.arp << 4), k))
                shown[(i, id(v))] = (base, note)
                holder[i] = v
                continue
            pb, pn = shown[(i, id(v))]
            if base != pb:
                ev.append((EV_BASE, i, base))
            if v.kind == "tone" and v.notes[k] != pn:
                ev.append((EV_SETNOTE, i, v.notes[k]))
            shown[(i, id(v))] = (base, v.notes[k] if v.kind == "tone" else pn)
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
            wait += 1
        while wait > 63:
            out.append(63); wait -= 63
        if wait:
            out.append(wait)
        out.append(0)
        segs.append(bytes(out))
    return segs


def song_blob(path, base_addr=0x8000, loop=True):
    """Bank image: header, order table, envelope table, envelopes, segments."""
    mod, envs, voices, segments, total = build(path)
    events = arrange(mod, voices, total)
    segs = encode(events, segments, total)
    uniq, order = [], []
    for s in segs:
        if s not in uniq:
            uniq.append(s)
        order.append(uniq.index(s))
    restart = 0
    # segment index of the module's restart order
    for i, (f0, n, oi) in enumerate(segments):
        if oi == mod.restart:
            restart = i
            break
    nenv = len(envs.tables)
    hdr = 4
    otab = hdr
    etab = otab + 2 * len(order)
    edata = etab + 2 * nenv
    env_addr, pos = [], edata
    for t in envs.tables:
        env_addr.append(pos); pos += len(t)
    seg_addr = []
    for s in uniq:
        seg_addr.append(pos); pos += len(s)
    blob = bytearray()
    blob += bytes([len(order), restart if loop else 0xFF]) + struct.pack("<H", base_addr + etab)
    for o in order:
        blob += struct.pack("<H", base_addr + seg_addr[o])
    for a in env_addr:
        blob += struct.pack("<H", base_addr + a)
    for t in envs.tables:
        blob += t
    for s in uniq:
        blob += s
    assert len(blob) == pos
    info = dict(frames=total, orders=len(order), unique=len(uniq), envs=nenv, size=len(blob),
                voices=len(voices))
    return bytes(blob), info


# ------------------------------------------------- reference player (model)
class RefPlayer:
    """Bit-exact model of src/music.c.  frame() returns the PSG bytes written."""

    def __init__(self, blob, base_addr=0x8000):
        self.b, self.base = blob, base_addr
        self.norders, self.restart = blob[0], blob[1]
        self.etab = struct.unpack_from("<H", blob, 2)[0] - base_addr
        self.order = 0
        self.stopped = False
        self.ptr = self.seg(0)
        self.wait = 0
        self.ch = [dict(on=False, note=0, arp=0, phase=0, base=0, env=0, run=0, att=15, hold=False,
                        last_att=15, last_per=0xFFFF) for _ in range(4)]

    def seg(self, i):
        return struct.unpack_from("<H", self.b, 4 + 2 * i)[0] - self.base

    def env_ptr(self, i):
        return struct.unpack_from("<H", self.b, self.etab + 2 * i)[0] - self.base

    def step_env(self, c):
        if c["hold"]:
            return
        if c["run"] == 0:
            e = self.b[c["env"]]
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
        b = self.b
        if self.stopped:
            return []
        if self.wait:
            self.wait -= 1
        else:
            while True:
                op = b[self.ptr]
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
                    self.ptr = self.seg(self.order)
                    continue
                if op < 0x40:
                    self.wait = op - 1
                    break
                i = op & 3
                c = self.ch[i]
                kind = op & 0xFC
                if kind in (0x40, 0x44):
                    c["note"] = b[self.ptr]
                    c["env"] = self.env_ptr(b[self.ptr + 1])
                    c["base"] = b[self.ptr + 2] & 15
                    c["arp"] = b[self.ptr + 2] >> 4
                    self.ptr += 3
                    c["on"], c["run"], c["hold"], c["phase"], c["att"] = True, 0, False, 0, 15
                    if kind == 0x44:
                        ofs = b[self.ptr]
                        self.ptr += 1
                        while ofs and c["on"] and not c["hold"]:
                            self.step_env(c)
                            ofs -= 1
                    if i == 3:
                        c["last_per"] = 0xFFFF      # rewrite the noise mode
                elif kind == 0x48:
                    c["on"] = False
                elif kind == 0x4C:
                    c["base"] = b[self.ptr] & 15
                    self.ptr += 1
                elif kind == 0x50:
                    c["note"] = b[self.ptr]
                    self.ptr += 1
        out = []
        for i, c in enumerate(self.ch):
            if c["on"]:
                self.step_env(c)
            att = min(15, c["att"] + c["base"]) if c["on"] else 15
            if i < 3:
                n = c["note"] + ARP_OFS[c["arp"]][c["phase"]]
                c["phase"] = c["phase"] + 1 if c["phase"] < 2 else 0
                per = PERIODS[n]
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


def reference_writes(blob, frames):
    p = RefPlayer(blob)
    return [p.frame() for _ in range(frames)]


if __name__ == "__main__":
    for p in sys.argv[1:]:
        blob, info = song_blob(p)
        print(os.path.basename(p), info)
