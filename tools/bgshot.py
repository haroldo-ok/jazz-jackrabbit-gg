#!/usr/bin/env python3
"""Hash (or save) the visible background of a smstest `dumpstate` file as
palette indices: nametable + tile data + scroll registers, so two builds
whose tile-cache slot assignments differ can still be compared pixel for
pixel.  Usage: bgshot.py dump [out.png]"""
import hashlib, sys
d = open(sys.argv[1], "rb").read()
vram, regs = d[:0x4000], d[0x4000 + 32:0x4000 + 48]
sx, sy = regs[8], regs[9]
nt = 0x3800
img = []
for y in range(24, 168):
    vy = (y + sy) % 224
    row = []
    for x in range(48, 208):
        vx = (x - sx) & 255
        e = vram[nt + ((vy >> 3) * 32 + (vx >> 3)) * 2] | (vram[nt + ((vy >> 3) * 32 + (vx >> 3)) * 2 + 1] << 8)
        t = e & 511
        px, py = vx & 7, vy & 7
        if e & 0x200: px = 7 - px
        if e & 0x400: py = 7 - py
        p = vram[t * 32 + py * 4: t * 32 + py * 4 + 4]
        row.append(sum(((p[k] >> (7 - px)) & 1) << k for k in range(4)) | ((e >> 11) & 1) << 4)
    img.append(bytes(row))
data = b"".join(img)
print(hashlib.md5(data).hexdigest())
if len(sys.argv) > 2:
    from PIL import Image
    im = Image.frombytes("L", (160, 144), bytes(v * 15 for v in data))
    im.save(sys.argv[2])
