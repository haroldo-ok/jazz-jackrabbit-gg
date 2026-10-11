#!/usr/bin/env python3
"""prof_report.py -- attribute an smstest `profdump` to functions.

smstest's sampling profiler (profstart / profstop / profdump <file>) writes one
line per 32-byte PC bucket: "<hex addr> <cycles>". This folds those buckets
onto the nearest preceding code symbol from the linker's .noi (DEF _name 0xADDR)
and prints a ranked table, so you can see *where a frame goes* instead of
guessing. Buckets in 0x8000-0xBFFF are banked code/data and are reported as
such (the .noi alone can't tell which bank was mapped).

Usage:
  prof_report.py prof.txt game.noi [--rst a.rst b.rst ...] [--frames N] [--top 20]

  --rst FILES  relocated listings (link with -Wl-u to get them). They carry
               EVERY function label, including `static` ones, which never
               reach the .noi -- without them, a static function's cycles are
               misattributed to the nearest public symbol before it (often a
               const data table).

  --frames N   frames covered by the profstart..profstop window, to print
               cycles/frame and % of a 59736-cycle NTSC frame.
"""
import argparse, re, bisect

NTSC_FRAME = 59736

def load_syms(noi):
    syms = []
    for line in open(noi):
        m = re.match(r'\s*DEF\s+(\S+)\s+0x([0-9A-Fa-f]+)', line)
        if not m:
            continue
        name, addr = m.group(1), int(m.group(2), 16)
        # keep code-ish symbols: skip RAM (>=0xC000), linker area/size markers
        if addr >= 0xC000 or name.startswith(('l_', 's_')) or '$' in name:
            continue
        syms.append((addr, name.lstrip('_')))
    return syms

RST_LABEL = re.compile(r'^\s+([0-9A-Fa-f]{4,6})\s+\d+\s+(_[A-Za-z_]\w*):')

def load_rst(path):
    out = []
    for line in open(path, errors='replace'):
        m = RST_LABEL.match(line)
        if m:
            addr = int(m.group(1), 16)
            if addr >= 0x10000 and 0x8000 <= (addr & 0xFFFF) < 0xC000:
                # code linked into a slot-2 bank (e.g. _BANK2=0x28000): the
                # only code that ever executes from 0x8000-0xBFFF
                out.append(((addr & 0xFFFF) | 0x1000000, m.group(2).lstrip('_')))
            elif addr < 0xC000:
                out.append((addr, m.group(2).lstrip('_')))
    return out

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('prof'); ap.add_argument('noi')
    ap.add_argument('--rst', nargs='*', default=[])
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--top', type=int, default=20)
    a = ap.parse_args()

    syms = load_syms(a.noi)
    for r in a.rst:
        syms += load_rst(r)
    syms = sorted(set(syms))
    banked = [(a & 0xFFFF, n) for a, n in syms if a & 0x1000000]
    syms = [(a, n) for a, n in syms if not a & 0x1000000]
    baddrs = [b[0] for b in banked]
    addrs = [s[0] for s in syms]
    per = {}
    total = 0
    for line in open(a.prof):
        p = line.split()
        if len(p) != 2:
            continue
        addr, cyc = int(p[0], 16), int(p[1])
        total += cyc
        if 0x8000 <= addr < 0xC000 and banked:
            j = bisect.bisect_right(baddrs, addr) - 1
            key = ('bank:' + banked[j][1]) if j >= 0 else '[banked 0x8000-0xBFFF]'
        elif 0x8000 <= addr < 0xC000:
            key = '[banked 0x8000-0xBFFF]'
        else:
            i = bisect.bisect_right(addrs, addr) - 1
            key = syms[i][1] if i >= 0 else '[before first symbol]'
        per[key] = per.get(key, 0) + cyc

    if not total:
        print('empty profile'); return
    print(f"{'share':>6} {'cycles':>11} {'cyc/frame':>10} {'%frame':>7}  symbol")
    for name, cyc in sorted(per.items(), key=lambda kv: -kv[1])[:a.top]:
        pf = cyc / a.frames if a.frames else 0
        pct = f'{100*pf/NTSC_FRAME:6.1f}%' if a.frames else '     -'
        pfs = f'{pf:10.0f}' if a.frames else '         -'
        print(f'{100*cyc/total:5.1f}% {cyc:11d} {pfs} {pct}  {name}')
    if a.frames:
        print(f'total: {total/a.frames:.0f} cycles/frame '
              f'({100*total/a.frames/NTSC_FRAME:.0f}% of an NTSC frame)')
        print('note: the total INCLUDES time spent waiting for VBlank, so it is '
              '~100% whenever the game keeps up.\n      The wait loop shows as a '
              'library symbol (e.g. near SMS_waitForVBlank) -- treat that row as '
              'idle headroom.\n      If the wait row is ~0% and the game still '
              'runs slow, the logic is overrunning the frame.')

if __name__ == '__main__':
    main()
