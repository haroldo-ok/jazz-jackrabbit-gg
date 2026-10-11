#!/usr/bin/env python3
"""lineprof.py cg.out game.noi file.rst [--frames N] [--func NAME]
Aggregate the call-graph dump's per-PC exclusive cycles by C source line
(using the ';src/x.c:NN: text' comments SDCC leaves in .rst listings)."""
import sys, re, collections
args = sys.argv[1:]
frames = 1
func = None
if '--frames' in args:
    i = args.index('--frames'); frames = int(args[i + 1]); del args[i:i + 2]
if '--func' in args:
    i = args.index('--func'); func = args[i + 1]; del args[i:i + 2]
dump, noi, rsts = args[0], args[1], args[2:]
per = {}
for l in open(dump):
    p = l.split()
    if p and p[0] == 'X':
        per[int(p[1], 16)] = int(p[2])
ins = re.compile(r'^\s+([0-9A-F]{6})\s+[0-9A-F ]+\s+\[\s*\d+\]\s+\d+\s+(.*)$')
src = re.compile(r'^\s+\d+\s+;(src/[^:]+:\d+):\s*(.*)$')
lab = re.compile(r'^\s+([0-9A-F]{6})\s+\d+\s+(_[A-Za-z_]\w*)::?')
tot = collections.Counter(); text = {}
for r in rsts:
    cur = None; curf = None
    for l in open(r, errors='replace'):
        m = lab.match(l)
        if m: curf = m.group(2).lstrip('_')
        m = src.match(l)
        if m:
            cur = m.group(1); text[cur] = m.group(2); continue
        m = ins.match(l)
        if m and cur:
            a = int(m.group(1), 16)
            if a >= 0x10000 and 0x8000 <= (a & 0xFFFF) < 0xC000: a &= 0xFFFF
            if func and curf != func: continue
            tot[cur] += per.get(a, 0)
for k, v in tot.most_common(25):
    print('%8d/f  %-22s %s' % (v // frames, k, text.get(k, '')[:70]))
