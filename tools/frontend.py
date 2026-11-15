"""Front-end and bonus-level assets for the Game Gear build.

Converted from the original game files (JJ1_DATA) with the Genesis port's
decoders (its tools/ directory):
  * the title picture (the last image of STARTUP.0SC), half size;
  * the main menu (MENU.000) and its six highlight states, half size;
  * the episode-selection pictures (MENU.000, one per row of the list);
  * the planet pictures (PLANET.###) for the "now approaching" screens;
  * the bonus levels (tools/bonus.py).

Pictures become Game Gear tiles in two 16-colour palettes (each tile uses the
one that suits it), deduplicated, with a nametable map per picture.
"""
import os
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

FE_TILE0 = 96          # VRAM tile where picture tiles start (font below)


def _gtools(packs):
    t = os.path.join(packs, "..", "tools")
    if t not in sys.path:
        sys.path.insert(0, t)


def snap(c):
    """8-bit RGB -> the nearest Game Gear colour (4 bits a channel)."""
    return tuple(min(255, ((v + 8) // 17) * 17) for v in c)


def gg12(c):
    r, g, b = (min(15, (v + 8) // 17) for v in c)
    return r | (g << 4) | (b << 8)


def _quantize(pixels, n):
    """Median cut of a list of RGB tuples to at most n GG colours."""
    arr = np.array(pixels, dtype=np.uint8).reshape(1, -1, 3)
    im = Image.fromarray(arr, "RGB")
    q = im.quantize(colors=n, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[:3 * n]
    cols = sorted(set(snap(tuple(pal[i:i + 3])) for i in range(0, len(pal), 3)))
    return cols


def picture(images, palettes=2):
    """Images (same size, multiple of 8) -> (tiles, maps, palettes).

    tiles: list of 32-byte planar tiles (deduplicated across the images)
    maps:  per image, nametable words (tile index from 0, palette bit 0x0800)
    palettes: `palettes` lists of 16 GG colours; each tile uses the one that
    reproduces it best."""
    w, h = images[0].size
    tw, th = w // 8, h // 8
    arrs = [np.array(im.convert("RGB"), dtype=np.int32) for im in images]
    tiles_rgb = []
    for a in arrs:
        for ty in range(th):
            for tx in range(tw):
                tiles_rgb.append(a[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8].reshape(64, 3))
    if palettes == 1:
        g = [0] * len(tiles_rgb)
    else:
        means = np.array([t.mean(axis=0) for t in tiles_rgb])
        sat = means.max(1) - means.min(1)
        centres = means[[int(np.argmin(sat)), int(np.argmax(sat))]].astype(float)
        for _ in range(10):
            g0 = np.argmin(((means[:, None, :] - centres[None]) ** 2).sum(2), axis=1)
            for k in range(2):
                if np.any(g0 == k):
                    centres[k] = means[g0 == k].mean(0)
        g = [int(x) for x in g0]
    pals = []
    for k in range(palettes):
        chosen = [tuple(p) for t, gg in zip(tiles_rgb, g) if gg == k for p in t] or [(0, 0, 0)]
        cols = (_quantize(chosen, 16) + [(0, 0, 0)] * 16)[:16]
        pals.append(cols)
    P = [np.array(p, dtype=np.int32) for p in pals]
    uniq = {}
    tiles = []
    maps = []
    per = tw * th
    for i, t in enumerate(tiles_rgb):
        best = None
        for k in range(len(P)):
            d = ((t[:, None, :] - P[k][None]) ** 2).sum(2)
            idx = d.argmin(1)
            err = d.min(1).sum()
            if best is None or err < best[0]:
                best = (err, k, idx)
        _, k, idx = best
        data = bytearray()
        for y in range(8):
            r = [int(v) for v in idx[y * 8:y * 8 + 8]]
            for plane in range(4):
                v = 0
                for x in range(8):
                    v |= ((r[x] >> plane) & 1) << (7 - x)
                data.append(v)
        data = bytes(data)
        if data not in uniq:
            uniq[data] = len(tiles)
            tiles.append(data)
        if i % per == 0:
            maps.append([])
        maps[-1].append(uniq[data] | (0x0800 if k == 1 else 0))
    return tiles, maps, [[gg12(c) for c in p] for p in pals]


def half_rgb(img):
    return img.resize((img.width // 2, img.height // 2), Image.LANCZOS)


def pad(img, w, h, colour=(0, 0, 0), at=(0, 0)):
    out = Image.new("RGB", (w, h), colour)
    out.paste(img, at)
    return out


class Out:
    def __init__(self, rom):
        self.rom = rom
        self.h = []
        self.c = []          # tables in the fixed ROM
        self.c3 = []         # tables in the front-end code bank
        self.c4 = []         # tables in the bonus code bank
        self.cur = self.c3

    def blob(self, name, data):
        bank, addr = self.rom.blob(bytes(data))
        self.h.append("#define %s_BANK %d" % (name, bank))
        self.h.append("#define %s_ADDR 0x%04X" % (name, addr))
        self.h.append("#define %s_SIZE %d" % (name, len(data)))
        return bank, addr

    def define(self, name, value):
        self.h.append("#define %s %s" % (name, value))

    def words(self, name, vals, typ="unsigned int"):
        self.h.append("extern const %s %s[%d];" % (typ, name, len(vals)))
        self.cur.append("const %s %s[%d] = { %s };" % (typ, name, len(vals), ", ".join("0x%04X" % v for v in vals)))

    def bytes_(self, name, vals):
        self.words(name, vals, "unsigned char")


def emit_picture(out, name, tiles, maps, pals, base=FE_TILE0):
    """Tiles blob, one map blob per image (words with the VRAM base added),
    palettes as fixed tables."""
    out.blob(name + "_TILES", b"".join(tiles))
    out.define(name + "_NTILES", len(tiles))
    for i, m in enumerate(maps):
        out.blob("%s_MAP%d" % (name, i), b"".join(struct.pack("<H", (v & 0x0800) | ((v & 0x7FF) + base)) for v in m))
    out.words(name.lower() + "_pal", [v for p in pals for v in p] + [0] * (32 - 16 * len(pals)))


# ------------------------------------------------------------------ title
def title(out, data, packs):
    _gtools(packs)
    from jj1_scene_decode import data_offsets, unpack_rle, u16
    from import_jj1_shareware import Reader, load_palette
    source = (data / "STARTUP.0SC").read_bytes()
    offsets = data_offsets(source)
    rr = Reader(source)
    rr.seek(offsets[3])
    pal = load_palette(rr)
    p = offsets[4]
    size = u16(source, p)
    w, h = u16(source, p + 3), u16(source, p + 5)
    pix = unpack_rle(source[p + 7:p + 2 + size], w * h)
    im = Image.new("RGB", (w, h))
    im.putdata([pal[c] for c in pix])
    im = pad(half_rgb(im), 160, 104)
    tiles, maps, pals = picture([im])
    emit_picture(out, "FE_TITLE", tiles, maps, pals)
    out.define("FE_TITLE_W", 20)
    out.define("FE_TITLE_H", 13)
    print("front end: title %d tiles" % len(tiles))


# ------------------------------------------------------------------ menu
MENU_RECTS = [(92, 35, 136, 22), (92, 57, 140, 22), (88, 83, 144, 22),
              (86, 109, 150, 23), (82, 137, 156, 26), (78, 166, 166, 29)]


def menu(out, data, packs):
    _gtools(packs)
    from import_jj1_shareware import decode_menu
    normal, bright = decode_menu(data / "MENU.000")
    pal = [tuple(normal.getpalette()[3 * i:3 * i + 3]) for i in range(256)]
    base = bytes(normal.getdata())
    hi = bytes(bright.getdata())
    images = []
    for x, y, w, h in MENU_RECTS:
        frame = bytearray(base)
        for py in range(y, y + h):
            for px in range(x, x + w):
                q = py * 320 + px
                if hi[q]:
                    frame[q] = hi[q]
        im = Image.new("RGB", (320, 200))
        im.putdata([pal[c] for c in frame])
        images.append(pad(half_rgb(im), 160, 104))
    tiles, maps, pals = picture(images)
    emit_picture(out, "FE_MENU", tiles, maps, pals)
    print("front end: menu %d tiles for 6 states" % len(tiles))
    assert FE_TILE0 + len(tiles) <= 448, len(tiles)


# ------------------------------------------------------------------ episode pictures
EP_TAGS = ["1", "2", "3", "4", "5", "6", "A", "B", "C", "X", "Z"]
EP_TITLES = ["TURTLE TERROR", "BALLISTIC BUNNY", "RABBITS REVENGE", "GENE MACHINE",
             "THE CHASE IS ON", "THE FINAL CLASH", "OUTTA DIS WORLD", "TURTLE SOUP",
             "WILD WABBIT", "HOLIDAY HARE", "BONUS LEVELS"]


def episode_pictures(out, data, packs, text_pal):
    _gtools(packs)
    from build_episode_menu import read_menu
    palette, pictures = read_menu(data / "MENU.000")
    for i, px in enumerate(pictures[:len(EP_TAGS)]):
        im = Image.new("RGB", (134, 110))
        im.putdata([palette[v] for v in px])
        im = pad(half_rgb(im), 72, 56, at=(2, 0))
        tiles, maps, pals = picture([im], palettes=1)
        # the picture uses the second palette (sprite palette); tiles are
        # loaded at FE_EPPIC
        assert len(tiles) <= 63
        out.blob("FE_EPPIC%d_TILES" % i, b"".join(tiles))
        out.blob("FE_EPPIC%d_MAP" % i, b"".join(struct.pack("<H", 0x0800 | ((v & 0x7FF) + FE_TILE0)) for v in maps[0]))
        out.words("fe_eppic%d_pal" % i, pals[0])
    out.define("FE_EPPICS", min(len(pictures), len(EP_TAGS)))
    out.h.append("extern const unsigned int *const fe_eppic_pal[%d];" % len(EP_TAGS))
    out.cur.append("const unsigned int *const fe_eppic_pal[%d] = { %s };" % (
        len(EP_TAGS), ", ".join("fe_eppic%d_pal" % i for i in range(len(EP_TAGS)))))
    banks = []
    for i in range(len(EP_TAGS)):
        banks.append("{ FE_EPPIC%d_TILES_BANK, FE_EPPIC%d_TILES_ADDR, FE_EPPIC%d_TILES_SIZE, FE_EPPIC%d_MAP_BANK, FE_EPPIC%d_MAP_ADDR }" % ((i,) * 5))
    out.h.append("typedef struct { unsigned char tb; unsigned int ta, ts; unsigned char mb; unsigned int ma; } FEPic;")
    out.h.append("extern const FEPic fe_eppic[%d];" % len(EP_TAGS))
    out.cur.append("const FEPic fe_eppic[%d] = { %s };" % (len(EP_TAGS), ", ".join(banks)))
    out.h.append("extern const char *const fe_ep_tag[%d];" % len(EP_TAGS))
    out.h.append("extern const char *const fe_ep_title[%d];" % len(EP_TAGS))
    out.cur.append("const char *const fe_ep_tag[%d] = { %s };" % (len(EP_TAGS), ", ".join('"%s"' % t for t in EP_TAGS)))
    out.cur.append("const char *const fe_ep_title[%d] = { %s };" % (len(EP_TITLES), ", ".join('"%s"' % t for t in EP_TITLES)))
    out.define("FE_EP_ROWS", len(EP_TAGS))
    print("front end: %d episode pictures" % len(pictures))


# ------------------------------------------------------------------ planets
def planets(out, data, packs, episodes, stages):
    _gtools(packs)
    from build_planets import planet
    seen = {}
    rows = []
    for e in episodes:
        meta_dir = None
    # planet files per episode, in stage order: the world number of the level
    import json
    for ei, e in enumerate(episodes):
        m = json.load(open(os.path.join(packs, "ep%d" % e["number"], "episode.json")))
        lst = []
        for ext in m["planets"]:
            f = data / ("PLANET." + ext)
            if not f.exists():
                lst.append(None)
                continue
            if ext not in seen:
                ident, name, cols, pix = planet(f)
                im = Image.new("RGB", (64, 55))
                im.putdata([cols[v] for v in pix])
                im = pad(im, 64, 56)
                tiles, maps, pals = picture([im], palettes=1)
                tb, ta = out.rom.blob(b"".join(tiles))
                mb, ma = out.rom.blob(b"".join(struct.pack("<H", 0x0800 | ((v & 0x7FF) + FE_TILE0)) for v in maps[0]))
                seen[ext] = (len(rows), name.upper())
                rows.append((tb, ta, len(tiles) * 32, mb, ma, pals[0], name.upper()))
            lst.append(seen[ext][0])
        e["planet_pics"] = lst
    out.h.append("extern const FEPic fe_planet[%d];" % len(rows))
    out.h.append("extern const unsigned int fe_planet_pal[%d][16];" % len(rows))
    out.h.append("extern const char *const fe_planet_name[%d];" % len(rows))
    out.cur.append("const FEPic fe_planet[%d] = { %s };" % (len(rows), ", ".join(
        "{ %d, 0x%04X, %d, %d, 0x%04X }" % r[:5] for r in rows)))
    out.cur.append("const unsigned int fe_planet_pal[%d][16] = { %s };" % (len(rows), ", ".join(
        "{ %s }" % ", ".join("0x%03X" % v for v in r[5]) for r in rows)))
    out.cur.append("const char *const fe_planet_name[%d] = { %s };" % (len(rows), ", ".join('"%s"' % r[6] for r in rows)))
    # per episode: planet picture of each planet index
    n = max(len(e["planet_pics"]) for e in episodes)
    out.h.append("extern const unsigned char fe_ep_planet[%d][%d];" % (len(episodes), n))
    out.cur.append("const unsigned char fe_ep_planet[%d][%d] = { %s };" % (len(episodes), n, ", ".join(
        "{ %s }" % ", ".join(str(255 if v is None else v) for v in (e["planet_pics"] + [None] * n)[:n]) for e in episodes)))
    out.define("FE_PLANETS", len(rows))
    print("front end: %d planet pictures" % len(rows))


# ------------------------------------------------------------------ sounds
def sounds(out, packs):
    import psgsfx
    psgsfx.build(out, os.path.join(packs, "..", "res", "jj1"))


def build(rom, data, packs, episodes, stages, rgb_to_gg, tile_bytes, half):
    out = Out(rom)
    data = Path(data)
    out.define("FE_TILE0", FE_TILE0)
    title(out, data, packs)
    menu(out, data, packs)
    episode_pictures(out, data, packs, None)
    planets(out, data, packs, episodes, stages)
    sounds(out, packs)
    import bonus
    out.cur = out.c4
    bonus.build(out, data, packs)
    return dict(header=out.h, fixed=out.c, bank3=out.c3, bank4=out.c4)
