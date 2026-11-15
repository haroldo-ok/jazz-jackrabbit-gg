"""JJ1 bonus levels for the Game Gear (episode 1's three, as on the Genesis).

From BONUSY.000 (floor tiles, horizon strip, palette), BONUS.000 (item and
player sprites) and BONUSMAP.000-002 (maps, events, settings, the player's
animations), read with the Genesis port's parser (tools/bonus_parse.py).

  texels  60 floor tiles at 16x16 (half the original 32x32), colour 1..15
  map n   256x256 tile grid as 8x8-tile chunks (1024 chunk numbers, then
          64-byte chunks), the 60 tiles' 8x8 collision cells (a bit each),
          the event grid as sparse rows (257 row starts, then (column, id))
  strip   the horizon picture, half size (256x10 in a 32x2-tile band)
  sprites the player's frames (half size, 5x6 tiles) and the four items at
          four sizes, in one 15-colour sprite palette
"""
import math
import os
import struct
import sys
from pathlib import Path

ITEMS = {1: 46, 2: 47, 3: 48, 4: 49}     # clock, gem, glove, exit
ITEM_PX = [16, 12, 8, 4]                 # half of 32, 24, 16, 8


def gg12(c):
    r, g, b = (min(15, (v + 8) // 17) for v in c)
    return r | (g << 4) | (b << 8)


def snap(c):
    return tuple(min(255, ((v + 8) // 17) * 17) for v in c)


def _quant(samples, n):
    from PIL import Image
    side = 512
    im = Image.new("RGB", (side, (len(samples) + side - 1) // side))
    im.putdata(samples + [samples[0]] * (side * im.height - len(samples)))
    q = im.quantize(colors=n, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    p = q.getpalette()[:3 * n]
    cols = []
    for i in range(0, len(p), 3):
        c = snap(tuple(p[i:i + 3]))
        if c not in cols:
            cols.append(c)
    return cols


def _nearest(cols, c, first=1):
    return first + min(range(len(cols)), key=lambda i: sum((cols[i][k] - c[k]) ** 2 for k in range(3)))


def tile_planar(rows):
    out = bytearray()
    for r in rows:
        for plane in range(4):
            v = 0
            for x in range(8):
                v |= ((r[x] >> plane) & 1) << (7 - x)
            out.append(v)
    return bytes(out)


def build(out, data, packs):
    sys.path.insert(0, os.path.join(packs, "..", "tools"))
    import bonus_parse as bp
    bg, pal, tiles = bp.parse_tileset(data / "BONUSY.000")
    sprites = bp.parse_sprites(data / "BONUS.000")
    levels = [bp.parse_level(data / ("BONUSMAP.%03d" % n)) for n in range(3)]

    # ---- floor/strip palette (entries 1..15; 0 is the sky)
    samples = [snap(pal[v]) for v in tiles] + [snap(pal[bg[(v // 512) * 832 + v % 512]]) for v in range(512 * 20)]
    fcols = _quant(samples, 15)
    cache = {}

    def fidx(orig):
        if orig not in cache:
            cache[orig] = _nearest(fcols, snap(pal[orig]))
        return cache[orig]
    floor_pal = [0] + [gg12(c) for c in fcols] + [0] * (15 - len(fcols))

    # ---- texels: 16x16 per tile, the top-left texel of each 2x2
    tex = bytearray()
    for t in range(60):
        for y in range(16):
            for x in range(16):
                q = [tiles[t * 1024 + (2 * y + dy) * 32 + 2 * x + dx] for dy in (0, 1) for dx in (0, 1)]
                best = max(set(q), key=q.count)
                tex.append(fidx(best))
    tb, ta = out.rom.blob(bytes(tex))
    assert ta == 0x8000
    out.define("BONUS_TEX_BANK", tb)

    # ---- collision cells: tile t, row r (0..7) -> bits for columns
    masks = bytearray(60 * 8)
    for t in range(60):
        for y in range(32):
            for x in range(32):
                if (tiles[t * 1024 + y * 32 + x] & 240) == 192:
                    masks[t * 8 + (y >> 2)] |= 1 << (x >> 2)

    # ---- maps
    metas = []
    for n, L in enumerate(levels):
        g = [min(59, t) for t in L["tiles"]]
        uniq, refs = {}, []
        for cy in range(0, 256, 8):
            for cx in range(0, 256, 8):
                ch = bytes(g[(cy + y) * 256 + cx + x] for y in range(8) for x in range(8))
                refs.append(uniq.setdefault(ch, len(uniq)))
        assert len(uniq) <= 256
        chunks = sorted(uniq, key=uniq.get)
        ev = L["events"]
        entries = bytearray()
        starts = []
        head = 1024 + 64 * len(chunks) + len(masks) + 2 * 257
        for y in range(256):
            starts.append(len(entries))
            for x in range(256):
                e = ev[y * 256 + x]
                if e:
                    entries += bytes([x, e])
        starts.append(len(entries))
        size = head + len(entries)
        bank, addr = out.rom.blob(bytes(size), align=64)
        blob = bytearray(bytes(refs))
        for c in chunks:
            blob += c
        blob += masks
        evbase = addr + head
        for s in starts:
            blob += struct.pack("<H", evbase + s)
        blob += entries
        assert len(blob) == size
        out.rom.put(bank, addr - 0x8000, bytes(blob))
        e = L["evt"]
        metas.append((bank, addr, addr + 1024, addr + 1024 + 64 * len(chunks),
                       addr + 1024 + 64 * len(chunks) + len(masks),
                       L["seconds"], L["items"], L["px"], L["py"],
                       [x["type"] for x in e], [x["passable"] for x in e],
                       [x["used"] for x in e], [x["sound"] for x in e]))
        print("bonus map %d: %d chunks, %d events, %d bytes" % (n, len(chunks), len(entries) // 2, size))
    out.h.append("""typedef struct {
    unsigned char bank;
    unsigned int refs, chunks, masks, evrows;
    unsigned int seconds, items;
    unsigned char startX, startY;
    unsigned char type[16], passable[16], used[16], sound[16];
} BonusMeta;""")
    out.h.append("extern const BonusMeta bonus_meta[3];")
    rows = []
    for m in metas:
        rows.append("{ %d, 0x%04X, 0x%04X, 0x%04X, 0x%04X, %d, %d, %d, %d, { %s }, { %s }, { %s }, { %s } }" % (
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8],
            ", ".join(map(str, m[9])), ", ".join(map(str, m[10])),
            ", ".join(map(str, m[11])), ", ".join(map(str, m[12]))))
    out.cur.append("const BonusMeta bonus_meta[3] = { %s };" % ", ".join(rows))
    out.words("bonus_floor_pal", floor_pal)

    # ---- sky gradient: six bands from the original's 128..171 ramp
    sky = [gg12(snap(pal[128 + round(k * 43 / 5)])) for k in range(6)]
    out.words("bonus_sky", sky)

    # ---- horizon strip: 512x20 -> 256x10, in a 256x16 band (3 rows of sky above)
    from PIL import Image
    strip_rgb = Image.new("RGB", (512, 20))
    strip_rgb.putdata([pal[bg[y * 832 + x]] for y in range(20) for x in range(512)])
    half_strip = strip_rgb.resize((256, 10), Image.NEAREST)
    band = [[0] * 256 for _ in range(16)]
    for y in range(10):
        for x in range(256):
            band[6 + y][x] = _nearest(fcols, snap(half_strip.getpixel((x, y))))
    st_uniq, st_map, st_tiles = {}, [], []
    for ty in range(2):
        for tx in range(32):
            t = tile_planar([band[ty * 8 + y][tx * 8:tx * 8 + 8] for y in range(8)])
            if t not in st_uniq:
                st_uniq[t] = len(st_tiles)
                st_tiles.append(t)
            st_map.append(st_uniq[t])
    out.blob("BONUS_STRIP_TILES", b"".join(st_tiles))
    out.define("BONUS_STRIP_NTILES", len(st_tiles))
    out.bytes_("bonus_strip_map", st_map)

    # ---- sprites: player frames and items in one palette
    seqs = []
    for L in levels:
        s = []
        for i in range(7):
            a = L["anims"][i * 64:(i + 1) * 64]
            s.append(list(a[7:7 + a[6]]))
        seqs.append(s)
    ids = sorted({j for s in seqs for q in s for j in q})
    ssamples = []
    for sid in ids + list(ITEMS.values()):
        w, h, px, m = sprites[sid]
        ssamples += [snap(pal[p]) for p, k in zip(px, m) if k and p]
    scols = _quant(ssamples, 15)
    spr_pal = [0] + [gg12(c) for c in scols] + [0] * (15 - len(scols))
    out.words("bonus_sprite_pal", spr_pal)
    scache = {}

    def sidx(orig):
        if orig not in scache:
            scache[orig] = _nearest(scols, snap(pal[orig]))
        return scache[orig]

    def sprite_image(sid):
        w, h, px, m = sprites[sid]
        return w, h, [[sidx(px[y * w + x]) if (m[y * w + x] and px[y * w + x]) else 0 for x in range(w)] for y in range(h)]

    def halve(img, w, h):
        hw, hh = (w + 1) // 2, (h + 1) // 2
        o = [[0] * hw for _ in range(hh)]
        for y in range(hh):
            for x in range(hw):
                q = [img[yy][xx] for yy in (2 * y, 2 * y + 1) for xx in (2 * x, 2 * x + 1) if yy < h and xx < w]
                s = [v for v in q if v]
                if len(s) >= 2:
                    o[y][x] = max(set(s), key=s.count)
        return o, hw, hh

    # player: half size, anchored like the original (top-left), 40x48
    frames = bytearray()
    for sid in ids:
        w, h, img = sprite_image(sid)
        hi, hw, hh = halve(img, w, h)
        canvas = [[0] * 40 for _ in range(48)]
        for y in range(min(hh, 48)):
            for x in range(min(hw, 40)):
                canvas[y][x] = hi[y][x]
        for ty in range(6):
            for tx in range(5):
                frames += tile_planar([canvas[ty * 8 + y][tx * 8:tx * 8 + 8] for y in range(8)])
    bank0 = None
    # frames: 960 bytes each; keep each frame inside one bank
    per_bank = 0x4000 // 960
    fb = []
    for i in range(0, len(ids), per_bank):
        chunk = frames[i * 960:(i + per_bank) * 960]
        b, a = out.rom.blob(bytes(chunk))
        for k in range(min(per_bank, len(ids) - i)):
            fb.append((b, a + k * 960))
    out.h.append("typedef struct { unsigned char bank; unsigned int addr; } BonusFrame;")
    out.h.append("extern const BonusFrame bonus_player_frame[%d];" % len(fb))
    out.cur.append("const BonusFrame bonus_player_frame[%d] = { %s };" % (len(fb), ", ".join("{ %d, 0x%04X }" % f for f in fb)))
    out.h.append("extern const unsigned char bonus_anim_count[3][7];")
    out.h.append("extern const unsigned char bonus_anim[3][7][19];")
    out.cur.append("const unsigned char bonus_anim_count[3][7] = { %s };" % ", ".join(
        "{ %s }" % ", ".join(str(len(q)) for q in s) for s in seqs))
    out.cur.append("const unsigned char bonus_anim[3][7][19] = { %s };" % ", ".join(
        "{ %s }" % ", ".join("{ %s }" % ", ".join(str(ids.index(j)) for j in q) for q in s) for s in seqs))

    # items: four sizes, each centred in its box: 16 (2x2), 12 (2x2), 8, 4 (1x1)
    from PIL import Image as I
    itiles = bytearray()
    for ev in (1, 2, 3, 4):
        w, h, img = sprite_image(ITEMS[ev])
        for size in ITEM_PX:
            im = I.new("P", (w, h), 0)
            im.putdata([v for r in img for v in r])
            sc = min(size / w, size / h)
            nw, nh = max(1, round(w * sc)), max(1, round(h * sc))
            im = im.resize((nw, nh), I.NEAREST)
            box = 16 if size > 8 else 8
            canvas = I.new("P", (box, box), 0)
            canvas.paste(im, ((box - nw) // 2, (box - nh) // 2))
            c = list(canvas.getdata())
            n = box // 8
            for ty in range(n):
                for tx in range(n):
                    itiles += tile_planar([c[(ty * 8 + y) * box + tx * 8:(ty * 8 + y) * box + tx * 8 + 8] for y in range(8)])
    out.blob("BONUS_ITEM_TILES", bytes(itiles))

    # floor rows: the original's perspective (rows y = 85 .. 1 above the
    # bottom, distance 800 / (92 - y)), 10 sample rows of 8 px on the GG.
    # Per row and heading (256 steps a turn) the width of the row,
    # d * cos(heading), in 1/8 px.
    side = []
    for row in range(10):
        yo = 85 - row * 84 // 9
        d = 819200 // (92 - yo)
        for a in range(65):          # a quarter turn; the rest by symmetry
            c = round(math.cos(a * 2 * math.pi / 256) * 1024)
            side.append((d * c) >> 12)   # d * cos in 1/256 px
    step = [int(v / 5) for v in side]     # (side * 8) / 40 samples, in 1/2048 px
    out.h.append("extern const signed int bonus_step[10 * 65];")
    out.cur.append("const signed int bonus_step[10 * 65] = { %s };" % ", ".join(map(str, step)))
    out.h.append("extern const signed int bonus_side[10 * 65];")
    out.cur.append("const signed int bonus_side[10 * 65] = { %s };" % ", ".join(map(str, side)))

    # sine table, 1024 steps a turn, amplitude 1024
    sine = [round(math.sin(i * 2 * math.pi / 1024) * 1024) for i in range(257)]
    out.h.append("extern const signed int bonus_sin[257];")
    out.cur.append("const signed int bonus_sin[257] = { %s };   /* a quarter turn */" % ", ".join(map(str, sine)))
    print("bonus: %d player frames, %d strip tiles, floor %d colours, sprites %d colours" % (
        len(ids), len(st_tiles), len(fcols), len(scols)))
