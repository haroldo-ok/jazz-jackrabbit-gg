#!/usr/bin/env python3
"""Decode a smstest `dumpstate` file: game struct + renderer caches."""
import os, re, struct, sys
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
off = dict(l.split() for l in open(os.path.join(ROOT, "gen/offsets.txt")))
off = {k: int(v) for k, v in off.items()}
syms = {}
for l in open(os.path.join(ROOT, "jazz.noi")):
    m = re.match(r"DEF (\S+) 0x([0-9A-Fa-f]+)", l)
    if m: syms[m.group(1)] = int(m.group(2), 16)
d = open(sys.argv[1], "rb").read()
ram = d[0x4000 + 32 + 16:]
G = syms["_game"] - 0xC000
def u8(a): return ram[a - 0xC000] if a >= 0xC000 else ram[G + a]
def s16(o): return struct.unpack_from("<h", ram, G + o)[0]
def u16(o): return struct.unpack_from("<H", ram, G + o)[0]
print("stage", ram[G + off["stage"]], "frame", u16(off["frame"]), "score", u16(off["score"]),
      "lives", ram[G + off["lives"]], "health", ram[G + off["health"]],
      "gems %d/%d" % (u16(off["stageGems"]), u16(off["stageGemTotal"])))
print("player x", s16(off["player.x"]), "y", s16(off["player.y"]), "vx", s16(off["player.vx"]),
      "vy", s16(off["player.vy"]), "ground", ram[G + off["player.onGround"]], "anim", ram[G + off["animState"]])
for i in range(6):
    b = G + off["enemies"] + i * off["size_enemy"]
    x, y, hx, hy, dr, act, kl, gx, gy, hp = struct.unpack_from("<hhhhbBBBBB", ram, b)
    if act: print(" enemy%d klass %d grid (%d,%d) x %d y %d dir %d hp %d" % (i, kl, gx, gy, x, y, dr, hp))
def arr(name, n, fmt="B"):
    a = syms.get("_" + name)
    if a is None: return None
    return struct.unpack_from("<%d%s" % (n, fmt), ram, a - 0xC000)
for n, cnt, f in [("enemyKey", 9, "H"), ("enemyAge", 9, "B"), ("itemKey", 8, "H"), ("enemyId", 6, "B")]:
    v = arr(n, cnt, f)
    if v: print(n, v)
