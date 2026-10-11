#!/usr/bin/env python3
"""Epic MegaGames MASI "new PSM" module reader and tick-level player model.

The format notes follow OpenMPT's Load_psm.cpp (BSD) and the ModdingWiki
"ProTracker Studio Module" page.  play() runs the song the way an S3M-style
tracker would and returns, for every tick, each channel's state: sample,
frequency in Hz, volume (0..64) and sample position -- enough to rebuild the
music on another sound chip.  render_wav() mixes the samples back from that
state so the model itself can be checked by ear against the original."""
import math, struct, sys

NOTE_CUT = 254


class Sample:
    def __init__(self, num, name, data, loop, loop_start, loop_end, c5, volume):
        self.num, self.name, self.data = num, name, data
        self.loop, self.loop_start, self.loop_end = loop, loop_start, loop_end
        self.c5, self.volume = c5, volume

    def __repr__(self):
        return "S%02d %-22s len %6d %s c5 %5d vol %2d" % (
            self.num, self.name, len(self.data),
            ("loop %d-%d" % (self.loop_start, self.loop_end)) if self.loop else "one-shot",
            self.c5, self.volume)


class Cell:
    __slots__ = ("note", "instr", "vol", "cmd", "param")

    def __init__(self):
        self.note = self.instr = self.vol = None
        self.cmd = self.param = None


class Module:
    def __init__(self, path):
        d = open(path, "rb").read()
        assert d[:4] == b"PSM " and d[8:12] == b"FILE", path
        self.title = ""
        self.patterns = {}
        self.samples = {}
        self.order, self.restart = [], 0
        self.speed, self.tempo = 6, 125
        self.channels = 0
        self.chan_volume = {}
        pos = 12
        while pos + 8 <= len(d):
            cid, ln = d[pos:pos + 4], struct.unpack_from("<I", d, pos + 4)[0]
            body = d[pos + 8:pos + 8 + ln]
            pos += 8 + ln
            if cid == b"TITL":
                self.title = body.split(b"\0")[0].decode("latin-1").strip()
            elif cid == b"SONG":
                self._song(body)
            elif cid == b"PBOD":
                self._pattern(body)
            elif cid == b"DSMP":
                self._sample(body)

    def _song(self, b):
        if self.order:
            return                         # only the first subsong
        self.channels = b[10]
        p = 11
        while p + 8 <= len(b):
            cid, ln = b[p:p + 4], struct.unpack_from("<I", b, p + 4)[0]
            sub = b[p + 8:p + 8 + ln]
            p += 8 + ln
            if cid != b"OPLH":
                continue
            q, count, first = 2, 0, None
            while q < len(sub):
                op = sub[q]; q += 1
                if op == 0:
                    break
                if op == 1:
                    pid = sub[q:q + 4].decode("latin-1"); q += 4
                    self.order.append(int(pid[1:].strip()))
                    if first is None:
                        first = count
                elif op == 2:
                    q += 4
                elif op in (3, 4):
                    r = struct.unpack_from("<H", sub, q)[0]; q += 2
                    if first is not None and r >= first:
                        self.restart = r - first
                    if op == 3:
                        q += 1
                elif op == 5:
                    q += 2
                elif op == 6:
                    q += 1
                elif op == 7:
                    self.speed = sub[q]; q += 1
                elif op == 8:
                    self.tempo = sub[q]; q += 1
                elif op == 0x0C:
                    q += 6
                elif op == 0x0D:
                    q += 3
                elif op == 0x0E:
                    self.chan_volume[sub[q]] = sub[q + 1] // 4 + 1; q += 2
                else:
                    raise ValueError("OPLH opcode %02x" % op)
                count += 1

    def _pattern(self, b):
        assert struct.unpack_from("<I", b, 0)[0] == len(b)
        pid = int(b[4:8].decode("latin-1")[1:].strip())
        rows = struct.unpack_from("<H", b, 8)[0]
        p = 10
        pat = []
        for _ in range(rows):
            size = struct.unpack_from("<H", b, p)[0]
            r = b[p + 2:p + size]
            p += size
            row = {}
            q = 0
            while q + 3 <= len(r):
                flags, ch = r[q], r[q + 1]; q += 2
                c = row.setdefault(ch, Cell())
                if flags & 0x80:
                    n = r[q]; q += 1
                    # OpenMPT note numbers: 1 = C-0
                    c.note = NOTE_CUT if n == 0xFF else (n & 15) + 12 * (n >> 4) + 13
                if flags & 0x40:
                    c.instr = r[q] + 1; q += 1
                if flags & 0x20:
                    c.vol = (min(r[q], 127) + 1) // 2; q += 1
                if flags & 0x10:
                    c.cmd, c.param = r[q], r[q + 1]; q += 2
                    if c.cmd == 0x29:
                        c.param = r[q]; q += 2
                    elif c.cmd == 0x33:
                        q += 1
            pat.append(row)
        self.patterns[pid] = pat

    def _sample(self, b):
        flags = b[0]
        name = b[13:46].split(b"\0")[0].decode("latin-1").strip()
        num, length, ls, le = struct.unpack_from("<HIII", b, 52)
        vol = b[68]
        c5 = struct.unpack_from("<I", b, 73)[0] & 0xFFFF
        raw = b[96:96 + length]
        acc, data = 0, []
        for v in raw:
            acc = (acc + v) & 255
            data.append(acc - 256 if acc > 127 else acc)
        loop = bool(flags & 0x80)
        if le:
            le += 1
        le = min(le, length)
        ls = min(ls, le)
        self.samples[num + 1] = Sample(num + 1, name, data, loop and le > ls, ls, le, c5,
                                       min(64, (vol + 1) * 2))


# ------------------------------------------------------------------ player
SINE = [int(round(255 * math.sin(math.pi * i / 32))) for i in range(64)]   # MOD-style, -255..255
AMIGA = 3546895 * 4   # MOD period clock x4 (periods kept in 1/4 MOD units, like S3M)


def note_freq(note, c5):
    """OpenMPT note 61 = C-5 plays the sample at its c5 rate."""
    return c5 * 2.0 ** ((note - 61) / 12.0)


class Chan:
    def __init__(self):
        self.sample = None
        self.period = 0.0      # S3M-style period: freq = AMIGA / period
        self.target = 0.0
        self.vol = 0
        self.pos = 0.0         # sample position (in sample frames)
        self.on = False
        self.mem = {}
        self.vib_pos = 0
        self.trem_pos = 0
        self.note = None
        self.arp = 0
        self.vib = 0.0
        self.trem = 0
        self.cut_tick = self.delay_tick = None
        self.pending = None
        self.retrig = 0
        self.trig = False


def play(mod, max_ticks=200000, loops=1):
    """Yield (tick_seconds, [channel states]) for each tick until the song has
    played `loops` times through (restart handled), plus the order position at
    which each row started.  A channel state is None (silent) or a tuple
    (sample_num, freq_hz, vol_0_64, pos, note_number)."""
    chans = [Chan() for _ in range(mod.channels)]
    for c, v in mod.chan_volume.items():
        pass
    speed, tempo = mod.speed, mod.tempo
    order_i, row_i = 0, 0
    played = 0
    visited = set()
    out = []
    rowmarks = []
    loop_row, loop_count = {}, {}
    while len(out) < max_ticks:
        if order_i >= len(mod.order):
            order_i = mod.restart
            played += 1
            if played >= loops:
                break
        pat = mod.patterns[mod.order[order_i]]
        if row_i >= len(pat):
            order_i += 1
            row_i = 0
            continue
        key = (order_i, row_i)
        rowmarks.append((len(out), order_i, row_i))
        row = pat[row_i]
        next_order, next_row = order_i, row_i + 1
        pat_delay = 0
        # ---- tick 0 processing
        for ch, c in enumerate(chans):
            cell = row.get(ch)
            c.cut_tick = c.delay_tick = None
            c.arp = 0
            c.vib = 0.0
            c.trem = 0
            c.retrig = 0
            c.cmd = None
            if cell is None:
                continue
            cmd, param = cell.cmd, cell.param
            c.cmd, c.param = cmd, param
            if cmd == 0x2C and (param & 15):            # note delay
                c.delay_tick = param & 15
                c.pending = cell
                continue
            trigger_cell(mod, c, cell)
            tick0_effect(c, cell)
            if cmd == 0x3D and param:
                speed = param
            elif cmd == 0x3E and param >= 32:
                tempo = param
            elif cmd == 0x34:
                next_order, next_row = order_i + 1, 0
            elif cmd == 0x35:                          # pattern loop
                x = param & 15
                if x == 0:
                    loop_row[ch] = row_i
                else:
                    n = loop_count.get(ch, 0)
                    if n < x:
                        loop_count[ch] = n + 1
                        next_order, next_row = order_i, loop_row.get(ch, 0)
                    else:
                        loop_count[ch] = 0
            elif cmd == 0x36:
                pat_delay = param & 15
        ticks = speed * (1 + pat_delay)
        for t in range(ticks):
            tt = t % speed
            if t:
                for c in chans:
                    tick_effect(mod, c, tt)
            secs = 2.5 / tempo
            state = []
            for c in chans:
                if not c.on or c.sample is None:
                    state.append(None)
                    continue
                s = mod.samples.get(c.sample)
                if s is None or not s.data:
                    state.append(None)
                    continue
                per = c.period + c.vib
                if per <= 0:
                    state.append(None)
                    continue
                f = AMIGA / per * 2.0 ** (c.arp / 12.0)
                v = max(0, min(64, c.vol + c.trem))
                state.append((c.sample, f, v, c.pos, c.note, c.trig))
                c.trig = False
                # advance the sample position over this tick
                c.pos += f * secs
                if c.pos >= s.loop_end if s.loop else c.pos >= len(s.data):
                    if s.loop:
                        span = s.loop_end - s.loop_start
                        c.pos = s.loop_start + (c.pos - s.loop_start) % span
                    else:
                        c.on = False
            out.append((secs, state))
        if next_row >= len(pat) and next_order == order_i:
            next_order, next_row = order_i + 1, 0
        if next_order != order_i:
            loop_row.clear()
        order_i, row_i = next_order, next_row
    return out, rowmarks


def trigger_cell(mod, c, cell):
    s = mod.samples.get(cell.instr) if cell.instr else None
    if cell.instr and s is not None:
        c.sample = cell.instr
        c.vol = s.volume
    if cell.note is not None:
        if cell.note == NOTE_CUT:
            c.on = False
        elif c.sample is not None and cell.note < 120:
            s = mod.samples.get(c.sample)
            if s is not None and s.c5:
                per = AMIGA / note_freq(cell.note, s.c5)
                if cell.cmd in (0x0F, 0x10, 0x12) and c.on:
                    c.target = per                       # tone portamento
                else:
                    c.period = per
                    c.target = per
                    c.pos = 0.0
                    c.on = True
                    c.vib_pos = 0
                    c.trig = True
                c.note = cell.note
    if cell.vol is not None:
        c.vol = cell.vol


def mem(c, cmd, param):
    if param:
        c.mem[cmd] = param
    return c.mem.get(cmd, 0)


def tick0_effect(c, cell):
    cmd, p = cell.cmd, cell.param
    if cmd is None:
        return
    if cmd == 0x01:                       # fine volslide up
        c.vol = min(64, c.vol + (((p & 0x1E) << 3) >> 4))
    elif cmd == 0x03:                     # fine volslide down
        c.vol = max(0, c.vol - (p >> 1))
    elif cmd in (0x0B, 0x0D):             # fine porta
        x = (p | 0xF0) & 15 if p < 4 else (p >> 2)
        amt = x * 4 if p >= 4 else x
        c.period += -amt if cmd == 0x0B else amt
    elif cmd == 0x29:                     # sample offset
        c.pos = p * 256
        c.trig = True
    elif cmd == 0x2B:
        c.cut_tick = p & 15
        if c.cut_tick == 0:
            c.on = False
    elif cmd in (0x02, 0x04, 0x0C, 0x0E, 0x0F, 0x15, 0x17, 0x18, 0x1F, 0x47, 0x2A, 0x10, 0x12):
        mem(c, cmd, p)
    if cmd == 0x47:
        c.arp = 0


def tick_effect(mod, c, t):
    cmd = c.cmd
    if c.delay_tick is not None and t == c.delay_tick:
        trigger_cell(mod, c, c.pending)
        c.delay_tick = None
    if c.cut_tick is not None and t == c.cut_tick:
        c.on = False
    if cmd is None:
        return
    p = c.mem.get(cmd, 0)
    if cmd == 0x02:                       # volslide up
        c.vol = min(64, c.vol + ((p << 3) >> 4 & 15))
    elif cmd == 0x04:                     # volslide down
        c.vol = max(0, c.vol - (p >> 1 & 15 if p >= 2 else p))
    elif cmd in (0x0C, 0x0E):             # porta up / down
        amt = (p >> 2) * 4 if p >= 4 else p
        c.period += -amt if cmd == 0x0C else amt
        c.target = c.period
    elif cmd in (0x0F, 0x10, 0x12):       # tone portamento (+volslide)
        sp = c.mem.get(0x0F, 0) if cmd != 0x0F else p
        sp = (sp >> 2) * 4 if cmd == 0x0F else sp * 4
        if c.period < c.target:
            c.period = min(c.target, c.period + sp)
        else:
            c.period = max(c.target, c.period - sp)
        if cmd == 0x10:
            c.vol = min(64, c.vol + ((p >> 4) & 15))
        elif cmd == 0x12:
            c.vol = max(0, c.vol - ((p >> 4) & 15))
    elif cmd in (0x15, 0x17, 0x18):       # vibrato (+volslide)
        vp = p if cmd == 0x15 else c.mem.get(0x15, 0)
        spd, dep = vp >> 4, vp & 15
        c.vib = SINE[c.vib_pos & 63] * dep / 128.0 * 4
        c.vib_pos += spd
        if cmd == 0x17:
            c.vol = min(64, c.vol + (p & 15))
        elif cmd == 0x18:
            c.vol = max(0, c.vol - (p & 15))
    elif cmd == 0x1F:                     # tremolo
        spd, dep = p >> 4, p & 15
        c.trem = SINE[c.trem_pos & 63] * dep // 64
        c.trem_pos += spd
    elif cmd == 0x47:                     # arpeggio
        x, y = p >> 4, p & 15
        c.arp = (0, x, y)[t % 3]
    elif cmd == 0x2A:                     # retrigger
        if (p & 15) and t % (p & 15) == 0:
            c.pos = 0.0
            c.on = True
            c.trig = True


def render_wav(mod, ticks, path, rate=22050):
    """Mix the samples from the tick states (sanity check of the model)."""
    import array, wave
    buf = array.array("h")
    phase = {}
    for secs, state in ticks:
        n = int(round(secs * rate))
        mix = [0.0] * n
        for ch, st in enumerate(state):
            if st is None:
                continue
            smp, f, v, pos, note = st[:5]
            s = mod.samples[smp]
            step = f / rate
            p = pos
            amp = v / 64.0 / 128.0
            data = s.data
            for i in range(n):
                ip = int(p)
                if s.loop and ip >= s.loop_end:
                    p = s.loop_start + (p - s.loop_start) % (s.loop_end - s.loop_start)
                    ip = int(p)
                if ip >= len(data):
                    break
                mix[i] += data[ip] * amp
                p += step
        for m in mix:
            buf.append(max(-32767, min(32767, int(m * 32767 / 3))))
    w = wave.open(path, "wb")
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
    w.writeframes(buf.tobytes())
    w.close()


if __name__ == "__main__":
    m = Module(sys.argv[1])
    print("title %r channels %d speed %d tempo %d orders %d restart %d patterns %d" % (
        m.title, m.channels, m.speed, m.tempo, len(m.order), m.restart, len(m.patterns)))
    print("order", m.order)
    for s in sorted(m.samples.values(), key=lambda s: s.num):
        print(" ", s)
    cmds = {}
    for pat in m.patterns.values():
        for row in pat:
            for c in row.values():
                if c.cmd is not None:
                    cmds[c.cmd] = cmds.get(c.cmd, 0) + 1
    print("effects", {("%02x" % k): v for k, v in sorted(cmds.items())})
    ticks, rows = play(m)
    print("duration %.1f s, %d ticks" % (sum(t[0] for t in ticks), len(ticks)))
