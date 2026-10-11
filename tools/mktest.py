#!/usr/bin/env python3
"""Expand @field (JazzGame field) and @enemy[i].field references in a test
template into RAM addresses for smstest.  Usage: mktest.py in.tpl > out.txt"""
import os, re, sys
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
off = {k: int(v) for k, v in (l.split() for l in open(os.path.join(ROOT, "gen/offsets.txt")))}
game = None
for l in open(os.path.join(ROOT, "jazz.noi")):
    m = re.match(r"DEF _game 0x([0-9A-Fa-f]+)", l)
    if m: game = int(m.group(1), 16)
EOFF = {"x": 0, "y": 2, "homeX": 4, "homeY": 6, "direction": 8, "active": 9, "klass": 10,
        "gridX": 11, "gridY": 12, "hitPoints": 13}
def sub(m):
    name = m.group(1)
    em = re.match(r"enemy\[(\d)\]\.(\w+)", name)
    if em:
        return "0x%04X" % (game + off["enemies"] + int(em.group(1)) * off["size_enemy"] + EOFF[em.group(2)])
    return "0x%04X" % (game + off[name])
for line in open(sys.argv[1]):
    sys.stdout.write(re.sub(r"@([\w.\[\]]+)", sub, line))
