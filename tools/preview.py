#!/usr/bin/env python3
"""Render previews straight from gen/data.bin (validates the bank formats)."""
import os, re, sys
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
G = os.path.join(ROOT, "gen")
blob = open(os.path.join(G, "data.bin"), "rb").read()
hdr = open(os.path.join(G, "gg_data.h")).read()
DATA_BANK0 = int(re.search(r"DATA_BANK0 (\d+)", hdr).group(1))
fixed = open(os.path.join(G, "gg_fixed.c")).read()


def bank(n):
    o = (n - DATA_BANK0) * 0x4000
    return blob[o:o + 0x4000]


def parse_rows(name):
    m = re.search(name + r"\[[^\]]*\] = \{(.*?)\n\};", fixed, re.S)
    rows = re.findall(r"\{ ([^{}]*?), \{ ([^}]*) \} \}", m.group(1))
    return [([int(x, 0) for x in a.split(",")], [int(x, 0) for x in b.split(",")]) for a, b in rows]


worlds = parse_rows("gg_world_info")
stages = parse_rows("gg_stage_info")


def rgb(c):
    return ((c & 15) * 17, ((c >> 4) & 15) * 17, ((c >> 8) & 15) * 17)


def tile_pixels(data, off):
    px = [[0] * 8 for _ in range(8)]
    for y in range(8):
        p = data[off + y * 4: off + y * 4 + 4]
        for x in range(8):
            px[y][x] = sum(((p[k] >> (7 - x)) & 1) << k for k in range(4))
    return px


def world_tile(w, t):
    tb = worlds[w][0][0]
    return tile_pixels(bank(tb + (t >> 9)), (t & 511) * 32)


def render_level(stage, x0, y0, bw, bh, path):
    a, spal = stages[stage]
    cellBank, eventBank, miscBank, sprBank, world = a[:5]
    wpal = worlds[world][1]
    cm, misc = bank(cellBank), bank(miscBank)
    img = Image.new("RGB", (bw * 16, bh * 16))
    pix = img.load()
    for by in range(bh):
        for bx in range(bw):
            cid = cm[((y0 + by) << 8) + x0 + bx]
            blk = misc[cid]
            for q in range(4):
                ref = misc[0x1300 + (blk * 4 + q) * 2] | (misc[0x1300 + (blk * 4 + q) * 2 + 1] << 8)
                t = world_tile(world, ref & 0x1FFF)
                hf, vf = (ref >> 13) & 1, (ref >> 14) & 1
                for y in range(8):
                    for x in range(8):
                        c = t[7 - y if vf else y][7 - x if hf else x]
                        pix[bx * 16 + (q & 1) * 8 + x, by * 16 + (q >> 1) * 8 + y] = rgb(wpal[c])
    img = img.resize((img.width * 3, img.height * 3), Image.NEAREST)
    img.save(path)


def sprite_tile(stage, t):
    sb = stages[stage][0][3]
    return tile_pixels(bank(sb + (t >> 9)), (t & 511) * 32)


def render_sprites(stage, path):
    a, spal = stages[stage]
    misc = bank(a[2])
    descs = []
    for i in range(128):
        d = misc[0x1B00 + i * 8: 0x1B00 + i * 8 + 8]
        if d[4]:
            descs.append(("ev%d" % i, d))
    for i in range(12):
        d = misc[0x1F00 + i * 8: 0x1F00 + i * 8 + 8]
        if d[4]:
            descs.append(("p%d" % i, d))
    for i in range(18):
        d = misc[0x1F60 + i * 8: 0x1F60 + i * 8 + 8]
        if d[4]:
            descs.append(("s%d" % i, d))
    W = 640
    img = Image.new("RGB", (W, 1200), (40, 40, 60))
    pix = img.load()
    x = y = 2
    rowh = 0
    for name, d in descs:
        tile = d[0] | (d[1] << 8)
        left = d[2] | (d[3] << 8)
        frames, tw, th = d[4], d[5] & 15, d[5] >> 4
        for base in ([tile, left] if left != tile else [tile]):
            for f in range(frames):
                if x + tw * 8 > W:
                    x = 2; y += rowh + 2; rowh = 0
                for ty in range(th):
                    for tx in range(tw):
                        t = sprite_tile(stage, base + f * tw * th + ty * tw + tx)
                        for yy in range(8):
                            for xx in range(8):
                                c = t[yy][xx]
                                if c and y + ty * 8 + yy < img.height:
                                    pix[x + tx * 8 + xx, y + ty * 8 + yy] = rgb(spal[c])
                x += tw * 8 + 2
                rowh = max(rowh, th * 8)
    img = img.crop((0, 0, W, min(1200, y + rowh + 4)))
    img = img.resize((img.width * 2, img.height * 2), Image.NEAREST)
    img.save(path)


if __name__ == "__main__":
    os.makedirs(os.path.join(ROOT, "shots"), exist_ok=True)
    s = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    render_level(s, 0, 0, 40, 20, os.path.join(ROOT, "shots", "preview_level%d.png" % s))
    render_sprites(s, os.path.join(ROOT, "shots", "preview_sprites%d.png" % s))
