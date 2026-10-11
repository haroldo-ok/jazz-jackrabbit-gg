#!/usr/bin/env python3
"""Check a smstest `dumpstate` file's visible background against the level
data: rebuild what the camera should show from gen/data.bin (cell grid ->
render block -> pool tiles, destroyed cells as sky) and compare it with the
nametable + VRAM tiles actually on screen.  Catches tile-cache refcount
leaks/underflows (stale slots) after long play.  Usage: bgverify.py dump..."""
import os, re, struct, subprocess, sys
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import preview as P
off = {k: int(v) for k, v in (l.split() for l in open(os.path.join(ROOT, "gen/offsets.txt")))}
syms = {}
for l in open(os.path.join(ROOT, "jazz.noi")):
    m = re.match(r"DEF (\S+) 0x([0-9A-Fa-f]+)", l)
    if m: syms[m.group(1)] = int(m.group(2), 16)

def bgshot(path):
    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools/bgshot.py"), path, "/dev/null"],
                         capture_output=True)
    d = open(path, "rb").read()
    vram, regs = d[:0x4000], d[0x4000 + 32:0x4000 + 48]
    sx, sy = regs[8], regs[9]
    img = []
    for y in range(24, 168):
        vy = (y + sy) % 224
        for x in range(48, 208):
            vx = (x - sx) & 255
            a = 0x3800 + ((vy >> 3) * 32 + (vx >> 3)) * 2
            e = vram[a] | (vram[a + 1] << 8)
            t, px, py = e & 511, vx & 7, vy & 7
            if e & 0x200: px = 7 - px
            if e & 0x400: py = 7 - py
            p = vram[t * 32 + py * 4: t * 32 + py * 4 + 4]
            img.append(sum(((p[k] >> (7 - px)) & 1) << k for k in range(4)))
    return img

def expected(d):
    ram = d[0x4000 + 48:]
    G = syms["_game"] - 0xC000
    stage = ram[G + off["stage"]]
    camX = struct.unpack_from("<h", ram, syms["_camX"] - 0xC000)[0]
    camY = struct.unpack_from("<h", ram, syms["_camY"] - 0xC000)[0]
    n = ram[G + off["destroyedCount"]]
    dead = {(ram[G + off["destroyedX"] + i], ram[G + off["destroyedY"] + i]) for i in range(n)}
    a, _ = P.stages[stage]
    cellBank, miscBank, world = a[0], a[2], a[4]
    cm, misc = P.bank(cellBank), P.bank(miscBank)
    cache = {}
    img = []
    hx0, hy0 = camX >> 1, camY >> 1
    for sy in range(144):
        for sx in range(160):
            hx, hy = hx0 + sx, hy0 + sy
            bx, by = hx >> 4, hy >> 4
            if 0 <= bx < 256 and 0 <= by < 64 and (bx, by) not in dead:
                blk = misc[cm[(by << 8) + bx]]
            else:
                blk = 0
            q = ((hy >> 3) & 1) * 2 + ((hx >> 3) & 1)
            ref = misc[0x1300 + (blk * 4 + q) * 2] | (misc[0x1300 + (blk * 4 + q) * 2 + 1] << 8)
            tid = ref & 0x1FFF
            if tid not in cache: cache[tid] = P.world_tile(world, tid)
            t = cache[tid]
            px, py = hx & 7, hy & 7
            if ref & 0x2000: px = 7 - px
            if ref & 0x4000: py = 7 - py
            img.append(t[py][px])
    return stage, camX, camY, img

bad = 0
for path in sys.argv[1:]:
    d = open(path, "rb").read()
    stage, cx, cy, want = expected(d)
    got = bgshot(path)
    diff = sum(1 for a, b in zip(want, got) if a != b)
    print("%s stage %d cam %d,%d: %s" % (os.path.basename(path), stage + 1, cx, cy,
                                         "OK" if not diff else "%d pixels differ" % diff))
    bad += diff != 0
sys.exit(1 if bad else 0)
