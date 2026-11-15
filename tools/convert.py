#!/usr/bin/env python3
"""Convert every episode of the Genesis port's JJ1 data into Game Gear banks.

Inputs
  JJ_GENESIS_PACKS  the Genesis repository's packs/ directory (packs/ep1 is in
                    git; packs/ep2..ep10 come from `tools/build_packs.py
                    generate --input <JAZZ>` in the Genesis repository)
  JJ1_DATA          the game directory (the PSM songs, MENU.000, fonts, the
                    bonus levels): the CD/registered JAZZ directory, or the
                    shareware one for episode 1 only

Everything graphical is downscaled by exactly half:
  * background: each 32x32 JJ1 block becomes a 16x16 block (2x2 GG tiles);
    the halved 8x8 tiles are deduplicated per world (with H/V flips) and the
    ROM streams them through a 255-slot VRAM cache;
  * sprites: every frame is halved too; Jazz gets pre-mirrored left frames
    because the SMS/GG VDP cannot flip sprites.

Game logic keeps the original 32 px block coordinates; only the renderer
divides by two.  Collision is baked here into a per-level "cell id" map so the
Z80 does one lookup per probe: each cell id names a (render block, solid mask,
feet-only mask) combination, with one-way platforms and bridge decks folded in
exactly as jj1_runtime.c computes them.

ROM layout (16 KB banks from DATA_BANK0 on; see gg_data.h):
  * world tile pool  - every world's halved tiles, one linear array
  * sprite pool      - every episode's sprite frames, one linear array
  * stage banks      - one per stage, fixed layout (MISC_* addresses): cell id
                       tables, event records, sprite descriptors, the block
                       tile references and the event grid as sparse columns
  * cell maps        - the 256x64 cell-id map of each stage as 4x4-cell
                       chunks: 1024 chunk pointers + the distinct chunks
  * songs and the front-end/bonus assets, packed into the remaining banks

Output (gen/): data.bin (the banks), gg_data.h, gg_fixed.c
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import jjdata as J

OUT = os.path.join(HERE, "..", "gen")
GEN_PACKS = os.environ.get("JJ_GENESIS_PACKS",
                           os.path.join(HERE, "..", "..", "jazz-jackrabbit-for-sega-genesis", "packs"))
GEN_TOOLS = os.path.join(GEN_PACKS, "..", "tools")
JJ1_DATA = os.environ.get("JJ1_DATA", os.path.expanduser("~/jj1data"))
DATA_BANK0 = int(os.environ.get("DATA_BANK0", "5"))
ROM_BANKS = 256                     # 4 MB
BANK = 0x4000

# Event classes (jj1_events.h)
C_NONE, C_ITEM, C_WALK, C_FLY, C_HAZARD, C_SPRING, C_ONEWAY, C_END, C_DESTRUCT, \
    C_TUBE, C_BRIDGE, C_UNBOARD, C_BOSS = range(13)


# ----------------------------------------------------------------- colour
def gg_color(genesis):
    r, g, b = J.genesis_color_to_rgb(genesis)
    c = lambda v: (v * 15 + 3) // 7
    return c(r) | (c(g) << 4) | (c(b) << 8)


def rgb_to_gg(rgb):
    r, g, b = rgb
    q = lambda v: max(0, min(15, (v + 8) // 17))
    return q(r) | (q(g) << 4) | (q(b) << 8)


# ----------------------------------------------------------------- halving
def half(img, pal_rgb):
    """2x2 box reduction of an indexed image (0 = transparent).
    A destination pixel is solid when at least two source pixels are; it
    takes the majority colour, or the palette entry nearest the average."""
    h, w = (len(img) + 1) // 2, (len(img[0]) + 1) // 2
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            px = []
            for dy in (0, 1):
                for dx in (0, 1):
                    yy, xx = 2 * y + dy, 2 * x + dx
                    if yy < len(img) and xx < len(img[0]):
                        px.append(img[yy][xx])
            solid = [p for p in px if p]
            if len(solid) < 2:
                continue
            best = max(set(solid), key=solid.count)
            if solid.count(best) >= 2:
                out[y][x] = best
                continue
            r = sum(pal_rgb[p][0] for p in solid) / len(solid)
            g = sum(pal_rgb[p][1] for p in solid) / len(solid)
            b = sum(pal_rgb[p][2] for p in solid) / len(solid)
            out[y][x] = min(range(1, 16), key=lambda i: (pal_rgb[i][0] - r) ** 2 +
                            (pal_rgb[i][1] - g) ** 2 + (pal_rgb[i][2] - b) ** 2)
    return out


def pad_to_tiles(img):
    h, w = len(img), len(img[0])
    th, tw = (h + 7) // 8, (w + 7) // 8
    out = [[0] * (tw * 8) for _ in range(th * 8)]
    for y in range(h):
        out[y][:w] = img[y]
    return out, tw, th


def tile_bytes(t):
    """8x8 indexed -> 32 bytes SMS planar."""
    out = bytearray()
    for row in t:
        for plane in range(4):
            v = 0
            for x in range(8):
                v |= ((row[x] >> plane) & 1) << (7 - x)
            out.append(v)
    return bytes(out)


def split_tiles(img, tw, th):
    tiles = []
    for ty in range(th):
        for tx in range(tw):
            tiles.append(tuple(tuple(img[ty * 8 + y][tx * 8:tx * 8 + 8]) for y in range(8)))
    return tiles


def mirror(img):
    return [row[::-1] for row in img]


# ----------------------------------------------------------------- banks
class Rom:
    """Data banks.  Whole banks for fixed-layout tables; linear arrays that
    run across consecutive banks; small blobs packed first-fit."""

    def __init__(self):
        self.banks = []
        self.fill = []          # bytes used, for first-fit blobs

    def new(self):
        self.banks.append(bytearray(BANK))
        self.fill.append(BANK)
        return DATA_BANK0 + len(self.banks) - 1

    def put(self, bank, offset, data):
        b = self.banks[bank - DATA_BANK0]
        assert offset + len(data) <= BANK, (bank, offset, len(data))
        b[offset:offset + len(data)] = data

    def linear(self, data):
        """Consecutive banks from a fresh one; returns the first bank."""
        first = None
        for off in range(0, max(1, len(data)), BANK):
            bk = self.new()
            if first is None:
                first = bk
            chunk = data[off:off + BANK]
            self.put(bk, 0, chunk)
            self.fill[bk - DATA_BANK0] = len(chunk)
        return first

    def blob(self, data, align=1):
        """First fit into a partly used bank (never across banks); returns
        (bank, slot-2 address)."""
        n = len(data)
        assert n <= BANK, n
        for i, used in enumerate(self.fill):
            at = (used + align - 1) // align * align
            if at + n <= BANK:
                self.banks[i][at:at + n] = data
                self.fill[i] = at + n
                return DATA_BANK0 + i, 0x8000 + at
        bk = self.new()
        self.fill[bk - DATA_BANK0] = 0
        return self.blob(data, align)

    def image(self):
        return b"".join(bytes(b) for b in self.banks)


# ----------------------------------------------------------------- font
FONT = {
    '0': ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    '1': ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    '2': ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    '3': ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    '4': ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    '5': ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    '6': ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    '7': ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    '8': ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    '9': ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    'A': ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    'B': ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    'C': ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    'D': ["11100", "10010", "10001", "10001", "10001", "10010", "11100"],
    'E': ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    'F': ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    'G': ["01110", "10001", "10000", "10111", "10001", "10001", "01111"],
    'H': ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    'I': ["01110", "00100", "00100", "00100", "00100", "00100", "01110"],
    'J': ["00111", "00010", "00010", "00010", "00010", "10010", "01100"],
    'K': ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    'L': ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    'M': ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    'N': ["10001", "10001", "11001", "10101", "10011", "10001", "10001"],
    'O': ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    'P': ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    'Q': ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    'R': ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    'S': ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    'T': ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    'U': ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    'V': ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    'W': ["10001", "10001", "10001", "10101", "10101", "10101", "01010"],
    'X': ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
    'Y': ["10001", "10001", "10001", "01010", "00100", "00100", "00100"],
    'Z': ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
    '-': ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    '!': ["00100", "00100", "00100", "00100", "00100", "00000", "00100"],
    ':': ["00000", "01100", "01100", "00000", "01100", "01100", "00000"],
    '/': ["00001", "00010", "00010", "00100", "01000", "01000", "10000"],
    '.': ["00000", "00000", "00000", "00000", "00000", "01100", "01100"],
    ',': ["00000", "00000", "00000", "00000", "01100", "00100", "01000"],
    '<': ["00010", "00100", "01000", "10000", "01000", "00100", "00010"],
    '>': ["01000", "00100", "00010", "00001", "00010", "00100", "01000"],
    '\'': ["00100", "00100", "01000", "00000", "00000", "00000", "00000"],
    '*': ["00000", "01010", "11111", "11111", "01110", "00100", "00000"],  # heart
    '#': ["01110", "11111", "10101", "11111", "11011", "01110", "00000"],  # face
    '(': ["00010", "00100", "01000", "01000", "01000", "00100", "00010"],
    ')': ["01000", "00100", "00010", "00010", "00010", "00100", "01000"],
    '?': ["01110", "10001", "00001", "00010", "00100", "00000", "00100"],
    '+': ["00000", "00100", "00100", "11111", "00100", "00100", "00000"],
    '"': ["01010", "01010", "00000", "00000", "00000", "00000", "00000"],
    '&': ["01100", "10010", "10100", "01000", "10101", "10010", "01101"],
    '=': ["00000", "00000", "11111", "00000", "11111", "00000", "00000"],
    '@': ["00100", "01110", "11111", "00100", "00100", "00100", "00100"],  # up arrow
    '^': ["00100", "00100", "00100", "00100", "11111", "01110", "00100"],  # down arrow
    '%': ["11001", "11010", "00010", "00100", "01000", "01011", "10011"],
    '$': ["00000", "00000", "00000", "00000", "00000", "00000", "11111"],  # underline
}
FONT_CHARS = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-!:/.,<>'*#()?+\"&=@^%$"


# ----------------------------------------------------------------- episodes
def episodes():
    out = []
    for n in range(1, 11):
        d = os.path.join(GEN_PACKS, "ep%d" % n)
        if not os.path.exists(os.path.join(d, "episode.json")):
            continue
        meta = json.load(open(os.path.join(d, "episode.json")))
        stages = []
        for m in re.finditer(r"STAGE\((\d+),\s*(\d+),\s*(0x[0-9A-Fa-f]+|\d+)\)",
                             open(os.path.join(d, "jj1_episode_stages.inc")).read()):
            stages.append((int(m.group(2)), int(m.group(3), 0)))
        game = open(os.path.join(d, "pack_game.inc")).read()
        def lst(name):
            m = re.search(r"#define %s \{([^}]*)\}" % name, game)
            return [int(v) for v in m.group(1).split(",")]
        meta["dir"] = d
        meta["stage_world"] = [w for w, _ in stages]
        meta["stage_cycles"] = [c for _, c in stages]
        meta["stage_bonus"] = lst("PACK_STAGE_BONUS")
        meta["stage_planet"] = lst("PACK_STAGE_PLANET")
        ends = re.findall(r"^\s*(\d+),", open(os.path.join(d, "jj1_episode_end_scenes.inc")).read(), re.M)
        meta["stage_clear"] = [int(v) for v in ends] + [0] * 8
        out.append(meta)
    return out


# ----------------------------------------------------------------- worlds
def parse_cycles(world):
    text = open(os.path.join(J.SRC, "jj1_world%03d_tiles.inc" % world)).read()
    m = re.search(r"jj1_world%03d_cycles\[\d+\]\s*=\s*\{(.*?)\n\};" % world, text, re.S)
    out = []
    if not m:
        return out
    for c in re.finditer(r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*\{([^}]*)\}\s*\}", m.group(1)):
        slot, count, bit, step, d = (int(c.group(i)) for i in range(1, 6))
        cols = [int(v, 0) for v in c.group(6).split(",") if v.strip()]
        cols += [0] * (4 - len(cols))
        out.append((slot, count, bit, step, d, [gg_color(v) for v in cols]))
    return out


def convert_world(world):
    W = J.load("jj1_world%03d_tiles.inc" % world)
    words = W["jj1_world%03d_block_tiles" % world]
    gpal = W["jj1_world%03d_palette" % world]
    pal_rgb = [J.genesis_color_to_rgb(c) for c in gpal]
    nblk = len(words) // 128
    pool = {}
    pool_list = []
    block_refs = []
    empty = tuple(tuple([0] * 8) for _ in range(8))
    pool[empty] = 0
    pool_list.append(empty)
    for b in range(256):
        if b < nblk:
            img = J.tiles_to_image(words[b * 128:(b + 1) * 128], 4, 4, "row")
            hb = half(img, pal_rgb)
        else:
            hb = [[0] * 16 for _ in range(16)]
        refs = []
        for ty in range(2):
            for tx in range(2):
                t = tuple(tuple(r[tx * 8:tx * 8 + 8]) for r in hb[ty * 8:ty * 8 + 8])
                cands = [
                    (t, 0),
                    (tuple(tuple(r[::-1]) for r in t), 1),
                    (t[::-1], 2),
                    (tuple(tuple(r[::-1]) for r in t[::-1]), 3),
                ]
                found = None
                for c, f in cands:
                    if c in pool:
                        found = (pool[c], f)
                        break
                if found is None:
                    pool[t] = len(pool_list)
                    pool_list.append(t)
                    found = (pool[t], 0)
                tid, f = found
                assert tid < 0x2000
                refs.append(tid | ((f & 1) << 13) | ((f >> 1) << 14))
        block_refs.append(refs)
    gg = [gg_color(c) for c in gpal]
    gg[0] = 0x0F44 & 0x0FFF
    return dict(world=world, tiles=pool_list, pal=gg, block_refs=block_refs, nblk=nblk,
                cycles=parse_cycles(world))


# ----------------------------------------------------------------- levels
def bake_cells(stage, nblk):
    D = J.load("jj1_level%d_data.inc" % stage)
    blocks = D["jj1_level%d_blocks" % stage]
    events = D["jj1_level%d_events" % stage]
    masks = D["jj1_level%d_masks" % stage]
    es = J.eventset(stage)

    def klass(e):
        return es[e][0] if e < 128 else C_NONE

    def bridge_deck(cellX, cellY):
        blockY = cellY >> 3
        blockX = cellX >> 3
        px = cellX << 2
        py = cellY << 2
        scan = blockX
        while scan >= 0 and scan > blockX - 9:
            e = events[(blockY << 8) + scan]
            info = es[e] if e < 128 else (0, 0, 0, 0, 0)
            if info[0] == C_BRIDGE:
                startX = scan << 5
                span = info[1] * info[3]
                deckY = (blockY << 5) + info[2]
                if px >= startX and px < startX + span and py + 4 > deckY and py < deckY + 8:
                    return 1
            scan -= 1
        return 0

    combos = {}
    combo_list = []
    cellmap = bytearray(256 * 64)
    for by in range(64):
        for bx in range(256):
            i = (by << 8) + bx
            blk = blocks[i]
            k = klass(events[i])
            solid = []
            down = []
            near_bridge = any(klass(events[(by << 8) + s]) == C_BRIDGE
                              for s in range(max(0, bx - 8), bx + 1))
            for cy in range(8):
                sm = dm = 0
                for cx in range(8):
                    cellX, cellY = (bx << 3) + cx, (by << 3) + cy
                    bm = (blk < 240) and bool(masks[(blk << 3) + cy] & (1 << cx))
                    deck = bridge_deck(cellX, cellY) if near_bridge else 0
                    if k == C_ONEWAY:
                        s, d = 0, (deck or bm)
                    elif k == C_BRIDGE:
                        s, d = 0, deck
                    else:
                        s, d = bm, (deck or bm)
                    sm |= (1 << cx) if s else 0
                    dm |= (1 << cx) if d else 0
                solid.append(sm)
                down.append(dm)
            key = (blk if blk < nblk else 0, tuple(solid), tuple(down))
            if key not in combos:
                combos[key] = len(combo_list)
                combo_list.append(key)
            cellmap[i] = combos[key] & 0xFF
    if len(combo_list) > 256:
        raise SystemExit("stage %d: %d cell combos > 256" % (stage, len(combo_list)))
    return cellmap, combo_list, bytes(e if e < 128 else 0 for e in events), es, D


def chunk_map(cellmap):
    """4x4-cell chunks: 1024 little-endian chunk addresses (relative to the
    blob) then the distinct 16-byte chunks."""
    uniq = {}
    order = []
    for cy in range(16):
        for cx in range(64):
            ch = bytes(cellmap[((cy * 4 + y) << 8) + cx * 4 + x] for y in range(4) for x in range(4))
            if ch not in uniq:
                uniq[ch] = len(uniq)
            order.append(uniq[ch])
    chunks = sorted(uniq, key=uniq.get)
    return order, chunks


def frames_of(tiles, desc, pal_rgb, flipped=False, remap=None):
    tile, frames, tw, th, tleft = desc
    out = []
    for f in range(frames):
        img = J.sprite_cell_image(tiles, tile + f * tw * th, tw, th)
        if remap is not None:
            img = [[remap[p] for p in r] for r in img]
        if flipped:
            img = mirror(img)
        hi = half(img, pal_rgb)
        hi, htw, hth = pad_to_tiles(hi)
        out.append((hi, htw, hth))
    return out


class SpritePool:
    """One episode's sprite tiles.  A run of frames already in the pool as a
    contiguous sequence (the same Jazz animation in several levels, an enemy
    reusing another's frames) is stored once."""

    def __init__(self):
        self.tiles = []
        self.where = {}         # first tile of a frame -> positions

    def add_frames(self, frames):
        seq = []
        for img, tw, th in frames:
            seq.extend(split_tiles(img, tw, th))
        n = len(seq)
        for pos in self.where.get(seq[0], ()):
            if self.tiles[pos:pos + n] == seq:
                return pos
        base = len(self.tiles)
        self.tiles.extend(seq)
        for i, t in enumerate(seq):
            self.where.setdefault(t, []).append(base + i)
        return base


def sprite_desc(pool, gtiles, desc, pal_rgb, mirror_left=False, remap=None):
    """-> 8 bytes: tile u16, tileLeft u16, frames, (tw|th<<4), cellW/2, cellH/2"""
    if desc is None or desc[1] == 0:
        return bytes(8), (0, 0)
    tile, frames, tw, th, tleft = desc
    fr = frames_of(gtiles, desc, pal_rgb, remap=remap)
    htw, hth = fr[0][1], fr[0][2]
    base = pool.add_frames(fr)
    if mirror_left:
        left = pool.add_frames(frames_of(gtiles, desc, pal_rgb, flipped=True, remap=remap))
    elif tleft != tile:
        left = pool.add_frames(frames_of(gtiles, (tleft, frames, tw, th, tleft), pal_rgb, remap=remap))
    else:
        left = base
    b = bytes([base & 0xFF, base >> 8, left & 0xFF, left >> 8, frames,
               htw | (hth << 4), tw * 4, th * 4])
    return b, (htw, hth)


def spring_frames(words, pal_rgb, remap=None):
    out = []
    for v in range(3):
        img = J.tiles_to_image(words[v * 64:(v + 1) * 64], 4, 2, "col")
        if remap is not None:
            img = [[remap[p] for p in r] for r in img]
        hi = half(img, pal_rgb)
        hi, tw, th = pad_to_tiles(hi)
        out.append((hi, tw, th))
    return out


# ----------------------------------------------------------------- Jazz
# Every level carries the same Jazz animation (MAINCHAR.000), but the Genesis
# converter quantised each level's sprites to its own 15 colours, so the
# frames differ level by level.  Here Jazz is taken once (from the first
# level) and drawn in JAZZ_COLOURS fixed palette entries shared by every
# level; each level's other sprites get the remaining entries, chosen for
# them, so an episode's sprite pool holds Jazz only once.
JAZZ_COLOURS = 10


def _dist(a, b):
    return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2


def _reduce(hist, n):
    """Merge the closest colours (weighted) until n remain."""
    cols = [(list(c), w) for c, w in hist.items()]
    while len(cols) > n:
        best = None
        for i in range(len(cols)):
            for j in range(i + 1, len(cols)):
                d = _dist(cols[i][0], cols[j][0]) * min(cols[i][1], cols[j][1])
                if best is None or d < best[0]:
                    best = (d, i, j)
        _, i, j = best
        (ci, wi), (cj, wj) = cols[i], cols[j]
        w = wi + wj
        merged = [(ci[k] * wi + cj[k] * wj) / w for k in range(3)]
        # snap to the original colour of the heavier one (keeps exact hues)
        merged = ci if wi >= wj else cj
        cols[i] = (merged, w)
        del cols[j]
    return [tuple(int(v) for v in c) for c, _ in sorted(cols, key=lambda cw: -cw[1])]


class JazzArt:
    def __init__(self):
        self.ref = None          # (state descs, per-state RGB frames) of the first level
        self.colours = None

    def stage_states(self, V, P):
        """Per state: (desc, [RGB frame images]) of this level's Jazz."""
        gp = V[P + "pal1"]
        rgb = [J.genesis_color_to_rgb(c) for c in gp]
        out = []
        for st in range(12):
            d = V[P + "player_states"].get(st)
            if not d or not d[1]:
                out.append(None)
                continue
            tile, frames, tw, th, _ = d
            fr = []
            for f in range(frames):
                img = J.sprite_cell_image(V[P + "player_tiles"], tile + f * tw * th, tw, th)
                fr.append([[rgb[p] if p else None for p in r] for r in img])
            out.append((d, fr))
        return out

    def setup(self, V, P):
        states = self.stage_states(V, P)
        if self.ref is None:
            hist = {}
            for st in states:
                if st:
                    for img in st[1]:
                        for r in img:
                            for c in r:
                                if c is not None:
                                    hist[c] = hist.get(c, 0) + 1
            self.colours = _reduce(hist, JAZZ_COLOURS)
            self.ref = states
        return states

    def shape(self, st):
        return None if st is None else (st[0][1:4], [[[c is not None for c in r] for r in img] for img in st[1]])

    def frames(self, st, flipped):
        """Indexed (1..JAZZ_COLOURS) frames of a state, halved and padded."""
        pal = [(0, 0, 0)] + list(self.colours)
        out = []
        for img in st[1]:
            ix = [[0 if c is None else 1 + min(range(JAZZ_COLOURS), key=lambda k: _dist(c, self.colours[k]))
                   for c in r] for r in img]
            if flipped:
                ix = mirror(ix)
            hi = half(ix, pal + [(0, 0, 0)] * (16 - len(pal)))
            hi, htw, hth = pad_to_tiles(hi)
            out.append((hi, htw, hth))
        return out


JAZZ = JazzArt()


def stage_palette(V, P):
    """The level's sprite palette: Jazz's shared colours, then up to
    15 - JAZZ_COLOURS colours for everything else, and the map from the
    level's Genesis sprite palette to it."""
    gp = V[P + "pal1"]
    rgb = [J.genesis_color_to_rgb(c) for c in gp]
    hist = {}

    def count_tiles(words, n=None):
        for i in range(0, len(words) if n is None else min(len(words), n * 8)):
            w = words[i]
            for k in range(8):
                p = (w >> (28 - 4 * k)) & 15
                if p:
                    hist[p] = hist.get(p, 0) + 1
    count_tiles(V[P + "event_sprite_tiles"])
    count_tiles(V.get(P + "spring_tiles", []))
    jz = list(JAZZ.colours)
    extra = []
    free = 15 - JAZZ_COLOURS
    # greedily add the colour that removes the most weighted error
    for _ in range(free):
        best = None
        for p, w in hist.items():
            c = rgb[p]
            if c in jz or c in extra:
                continue
            gain = 0
            for q, wq in hist.items():
                cq = rgb[q]
                cur = min(_dist(cq, x) for x in jz + extra)
                gain += wq * max(0, cur - _dist(cq, c))
            if best is None or gain > best[0]:
                best = (gain, c)
        if best is None or best[0] <= 0:
            break
        extra.append(best[1])
    pal = [(0, 0, 0)] + jz + extra
    pal += [(0, 0, 0)] * (16 - len(pal))
    used = 1 + len(jz) + len(extra)
    remap = [0] + [min(range(1, used), key=lambda k: _dist(rgb[p], pal[k])) for p in range(1, 16)]
    return pal, remap


def rgb7_to_gg(c):
    q = lambda v: (v * 15 + 3) // 7
    return q(c[0]) | (q(c[1]) << 4) | (q(c[2]) << 8)


# Stage bank layout (slot-2 addresses 0x8000 + offset); see gg_data.h
OFF_CELLBLOCK = 0x0000
OFF_CELLSOLID = 0x0100
OFF_CELLDOWN = 0x0900
OFF_EVENTINFO = 0x1100
OFF_BLOCKREFS = 0x1300
OFF_EVSPRITES = 0x1B00
OFF_PLAYER = 0x1F00
OFF_SPECIALS = 0x1F60
OFF_BOSSOFFS = 0x1FF0
OFF_SPREMPTY = 0x3600
OFF_BLOCKFLIPS = 0x2200
OFF_EVSOUND = 0x2600
OFF_EVCOL = 0x2680
OFF_EVENTRIES = 0x2890


def convert_level(stage, world_info, pool, rom):
    W = world_info
    cellmap, combos, events, es, D = bake_cells(stage, W["nblk"])
    V = J.load("jj1_level%d_visuals.inc" % stage)
    P = "jj1_level%d_" % stage
    jstates = JAZZ.setup(V, P)
    pal_rgb, remap = stage_palette(V, P)

    misc = bytearray(BANK)

    def put(off, data):
        misc[off:off + len(data)] = data

    cellBlock = bytes(c[0] for c in combos) + bytes(256 - len(combos))
    cellSolid = b"".join(bytes(c[1]) for c in combos) + bytes(8 * (256 - len(combos)))
    cellDown = b"".join(bytes(c[2]) for c in combos) + bytes(8 * (256 - len(combos)))
    einfo = b"".join(bytes(e[:4]) for e in es)
    brefs = b"".join(r.to_bytes(2, "little") for b in W["block_refs"] for r in b)
    put(OFF_CELLBLOCK, cellBlock)
    put(OFF_CELLSOLID, cellSolid)
    put(OFF_CELLDOWN, cellDown)
    put(OFF_EVENTINFO, einfo)
    put(OFF_BLOCKREFS, brefs)
    bflips = bytes(((r >> 12) & 6) for b in W["block_refs"] for r in b)
    assert len(bflips) <= 0x400
    put(OFF_BLOCKFLIPS, bflips)
    put(OFF_EVSOUND, bytes(e[4] for e in es))

    # --- sprites
    gtiles = V[P + "event_sprite_tiles"]
    evsprites = V[P + "event_sprites"]
    descs = bytearray()
    maxdim = {}
    for i in range(128):
        d = evsprites.get(i)
        b, dim = sprite_desc(pool, gtiles, d, pal_rgb, remap=remap)
        descs += b
        if d and d[1]:
            maxdim[i] = dim
    put(OFF_EVSPRITES, bytes(descs))

    pdesc = bytearray()
    for s in range(12):
        st = jstates[s]
        if st is None:
            pdesc += bytes(8)
            continue
        # the first level's frames unless this level's differ in shape
        ref = JAZZ.ref[s]
        if ref is not None and JAZZ.shape(ref) == JAZZ.shape(st):
            st = ref
        d = st[0]
        fr = JAZZ.frames(st, False)
        base = pool.add_frames(fr)
        left = pool.add_frames(JAZZ.frames(st, True))
        htw, hth = fr[0][1], fr[0][2]
        pdesc += bytes([base & 0xFF, base >> 8, left & 0xFF, left >> 8, d[1],
                        htw | (hth << 4), d[2] * 4, d[3] * 4])
    put(OFF_PLAYER, bytes(pdesc))

    specials = bytearray()
    names = ["bird_sprite", "bird_left_sprite", "shield_sprite", "board_sprite",
             "board_left_sprite", "boss_cannon_sprite", "boss_cannon_left_sprite",
             "boss_late_sprite", "boss_late_left_sprite", "boss_spike_sprite",
             "boss_spike_left_sprite", "boss_shot_sprite"]
    for n in names:
        d = V.get(P + n)
        b, dim = sprite_desc(pool, gtiles, d, pal_rgb, remap=remap)
        specials += b
        maxdim[n] = dim
    sw = V.get(P + "spring_tiles")
    sbase = pool.add_frames(spring_frames(sw, pal_rgb, remap))
    for v in range(3):
        t = sbase + v * 2
        specials += bytes([t & 0xFF, t >> 8, t & 0xFF, t >> 8, 1, 2 | (1 << 4), 16, 8])
    offs = bytearray()
    for n in ["boss_cannon_x", "boss_cannon_y", "boss_cannon_left_x", "boss_cannon_left_y",
              "boss_spike_x", "boss_spike_y", "boss_spike_left_x", "boss_spike_left_y"]:
        offs += (V.get(P + n, 0) & 0xFFFF).to_bytes(2, "little")
    put(OFF_BOSSOFFS, bytes(offs))

    def rows_img(rows):
        return [[int(ch, 16) for ch in r] for r in rows]
    gem = rows_img(["00000000", "000AA000", "00A88A00", "0A8118A0",
                    "0A8118A0", "00A88A00", "000AA000", "00000000"])
    bullet = rows_img(["00000000", "00000000", "00033000", "00311300",
                       "00311300", "00033000", "00000000", "00000000"])
    bar = [[remap[3]] * 8 for _ in range(8)]
    extra = []
    for img in (gem, bullet):
        img = [[remap[p] for p in r] for r in img]
        hi, _, _ = pad_to_tiles(half(img, pal_rgb))
        t = [[0] * 8 for _ in range(8)]
        for y in range(4):
            for x in range(4):
                t[y + 2][x + 2] = hi[y][x]
        extra.append((t, 1, 1))
    extra.append((bar, 1, 1))
    ebase = pool.add_frames(extra)
    for k, (w, hgt) in enumerate([(4, 4), (4, 4), (8, 8)]):
        t = ebase + k
        specials += bytes([t & 0xFF, t >> 8, t & 0xFF, t >> 8, 1, 1 | (1 << 4), w, hgt])
    assert OFF_SPECIALS + len(specials) <= OFF_BOSSOFFS
    put(OFF_SPECIALS, bytes(specials))
    bright = max(range(1, 16), key=lambda i: sum(pal_rgb[i]))
    dark = min(range(1, 16), key=lambda i: sum(pal_rgb[i]))

    items = 0
    boss = None
    for i, e in enumerate(events):
        k = es[e][0]
        if k == C_ITEM:
            items += 1
        if k == C_BOSS and boss is None:
            boss = (i & 255, i >> 8)
    order, chunks = chunk_map(cellmap)
    eorder, echunks = chunk_map(events)
    return dict(misc=misc, order=order, chunks=chunks, eorder=eorder, echunks=echunks,
                startX=D[P + "start_x"], startY=D[P + "start_y"],
                items=items, boss=boss, spal=[rgb7_to_gg(c) for c in pal_rgb],
                bright=bright, dark=dark, ncombos=len(combos), nev=len(echunks))


# ----------------------------------------------------------------- music
SONG_ALIASES = {}


def song_file(name):
    for f in os.listdir(JJ1_DATA):
        if f.lower() == name.lower():
            return os.path.join(JJ1_DATA, f)
    return None


def level_music(level_file):
    """The module name a level file asks for (its first header string)."""
    sys.path.insert(0, GEN_TOOLS)
    from build_packs import level_strings
    from pathlib import Path
    return level_strings(Path(song_file(level_file)))[0]


# ----------------------------------------------------------------- main
def main():
    os.makedirs(OUT, exist_ok=True)
    rom = Rom()
    eps = episodes()
    assert eps, "no episodes under %s" % GEN_PACKS

    # ---- pass 1: worlds and stages (pools are laid out afterwards)
    worlds = {}          # world number -> info (global index in order)
    world_order = []
    stage_list = []      # global
    ep_info = []
    ep_pools = []
    for ep in eps:
        J.set_source(ep["dir"])
        pool = SpritePool()
        first_stage = len(stage_list)
        for s in range(ep["stages"]):
            wn = ep["stage_world"][s]
            key = (ep["episode"], wn)    # the same world number holds the same art
            if wn not in worlds:
                worlds[wn] = convert_world(wn)
                worlds[wn]["index"] = len(world_order)
                world_order.append(wn)
            st = convert_level(s, worlds[wn], pool, rom)
            st["world"] = worlds[wn]["index"]
            st["episode"] = len(ep_info)
            st["cycles"] = ep["stage_cycles"][s]
            st["bonus"] = ep["stage_bonus"][s]
            st["planet"] = ep["stage_planet"][s]
            st["clear"] = ep["stage_clear"][s]
            st["music"] = level_music(ep["levels"][s])
            st["level"] = ep["levels"][s]
            stage_list.append(st)
            print("ep %d stage %d (%s): world %d, %d combos, %d chunks, %d event chunks, pool %d, items %d, boss %s, song %s" % (
                ep["episode"], s, ep["levels"][s], wn, st["ncombos"], len(st["chunks"]), st["nev"],
                len(pool.tiles), st["items"], st["boss"], st["music"]))
        assert len(pool.tiles) <= 0x400 * 8, len(pool.tiles)
        ep_pools.append(pool)
        ep_info.append(dict(number=ep["episode"], title=ep["title"], worlds=ep["worlds"],
                            first=first_stage, count=ep["stages"]))

    # ---- world tile pool (linear)
    wdata = bytearray()
    for wn in world_order:
        w = worlds[wn]
        w["base"] = len(wdata) // 32
        for t in w["tiles"]:
            wdata += tile_bytes(t)
    world_bank0 = rom.linear(bytes(wdata))
    print("world pool: %d tiles, %d KB" % (len(wdata) // 32, len(wdata) // 1024))

    # ---- sprite pool (linear)
    sdata = bytearray()
    for e, pool in zip(ep_info, ep_pools):
        e["sprBase"] = len(sdata) // 32
        for t in pool.tiles:
            sdata += tile_bytes(t)
    spr_bank0 = rom.linear(bytes(sdata))
    print("sprite pool: %d tiles, %d KB" % (len(sdata) // 32, len(sdata) // 1024))

    # ---- stage banks (sprite-empty bitmap is per episode pool)
    for st in stage_list:
        pool = ep_pools[st["episode"]]
        empty = bytearray(0x400)
        for i, t in enumerate(pool.tiles):
            if not any(any(r) for r in t):
                empty[i >> 3] |= 1 << (i & 7)
        st["misc"][OFF_SPREMPTY:OFF_SPREMPTY + 0x400] = empty
        bk = rom.new()
        rom.put(bk, 0, bytes(st["misc"]))
        rom.fill[bk - DATA_BANK0] = BANK
        st["miscBank"] = bk

    # ---- cell and event maps (chunked), packed first-fit, largest first
    def place(order, chunks):
        size = 2048 + 16 * len(chunks)
        bank, addr = rom.blob(bytes(size), align=16)
        base = addr + 2048
        blob = bytearray()
        for o in order:
            blob += (base + 16 * o).to_bytes(2, "little")
        for c in chunks:
            blob += c
        rom.put(bank, addr - 0x8000, bytes(blob))
        return bank, addr
    jobs = []
    for st in stage_list:
        jobs.append((len(st["chunks"]), st, "cell"))
        jobs.append((len(st["echunks"]), st, "ev"))
    for _, st, kind in sorted(jobs, key=lambda j: -j[0]):
        if kind == "cell":
            st["cellBank"], st["cellAddr"] = place(st["order"], st["chunks"])
        else:
            st["evBank"], st["evAddr"] = place(st["eorder"], st["echunks"])

    # ---- music
    import psgmusic
    song_names = ["MENUSNG", "BOSS", "ENDLEVEL", "BONUS"]
    for st in stage_list:
        m = os.path.splitext(st["music"])[0].upper()
        if m not in song_names:
            song_names.append(m)
    songs = []
    for name in song_names:
        path = song_file(name + ".PSM")
        loop = name not in ("ENDLEVEL",)
        if not path:
            print("music: %s missing" % name)
            songs.append((0, 0))
            continue
        parts = psgmusic.song_parts(path, loop=loop)
        # segments anywhere, then the home block (order table, envelopes)
        locs = [rom.blob(sg) for sg in parts["segs"]]
        size = len(psgmusic.home_blob(parts, locs, 0x8000))
        bank, addr = rom.blob(bytes(size))
        rom.put(bank, addr - 0x8000, psgmusic.home_blob(parts, locs, addr))
        songs.append((bank, addr))
        print("music: %-10s %5d bytes, home %d:%04X, segments in %d banks" % (
            name, parts["info"]["size"], bank, addr, len({b for b, a in locs})))
    for st in stage_list:
        st["song"] = song_names.index(os.path.splitext(st["music"])[0].upper())
        if st["boss"]:
            st["song"] = song_names.index("BOSS")

    # ---- front end and bonus assets
    import frontend
    fe = frontend.build(rom, JJ1_DATA, GEN_PACKS, ep_info, stage_list, rgb_to_gg, tile_bytes, half)

    blob = rom.image()
    total_banks = DATA_BANK0 + len(rom.banks)
    assert total_banks <= ROM_BANKS, "ROM too big: %d banks" % total_banks
    open(os.path.join(OUT, "data.bin"), "wb").write(blob)

    font = bytearray()
    for ch in FONT_CHARS:
        rows = FONT.get(ch, ["00000"] * 7)
        for y in range(8):
            v = 0
            if y < 7:
                for x, c in enumerate(rows[y]):
                    if c == '1':
                        v |= 0x80 >> (x + 1)
            font.append(v)

    h = []
    h.append("/* Generated by tools/convert.py - do not edit. */")
    h.append("#ifndef GG_DATA_H\n#define GG_DATA_H")
    h.append("#define DATA_BANK0 %d" % DATA_BANK0)
    h.append("#define DATA_BANKS %d" % len(rom.banks))
    h.append("#define GG_STAGES %d" % len(stage_list))
    h.append("#define GG_EPISODES %d" % len(ep_info))
    h.append("#define GG_WORLDS %d" % len(world_order))
    h.append("#define WORLD_BANK0 %d" % world_bank0)
    h.append("#define SPR_BANK0 %d" % spr_bank0)
    for name, off, typ in [("CELLBLOCK", OFF_CELLBLOCK, "unsigned char"), ("CELLSOLID", OFF_CELLSOLID, "unsigned char"),
                           ("CELLDOWN", OFF_CELLDOWN, "unsigned char"), ("EVENTINFO", OFF_EVENTINFO, "unsigned char"),
                           ("BLOCKREFS", OFF_BLOCKREFS, "unsigned int"), ("EVSPRITES", OFF_EVSPRITES, "GGSprite"),
                           ("PLAYER", OFF_PLAYER, "GGSprite"), ("SPECIALS", OFF_SPECIALS, "GGSprite"),
                           ("BOSSOFFS", OFF_BOSSOFFS, "signed int"), ("SPREMPTY", OFF_SPREMPTY, "unsigned char"),
                           ("BLOCKFLIPS", OFF_BLOCKFLIPS, "unsigned char"), ("EVSOUND", OFF_EVSOUND, "unsigned char")]:
        h.append("#define MISC_%-10s ((const %s *)0x%04X)" % (name, typ, 0x8000 + off))
    h.append("#define TILE_POOL_MAX %d" % max(len(worlds[w]["tiles"]) for w in world_order))
    for i, n in enumerate(["BIRD", "BIRD_LEFT", "SHIELD", "BOARD", "BOARD_LEFT",
                           "BOSS_CANNON", "BOSS_CANNON_LEFT", "BOSS_LATE", "BOSS_LATE_LEFT",
                           "BOSS_SPIKE", "BOSS_SPIKE_LEFT", "BOSS_SHOT",
                           "SPRING0", "SPRING1", "SPRING2", "GEM", "BULLET", "BOSSBAR"]):
        h.append("#define SPECIAL_%s %d" % (n, i))
    for i, n in enumerate(song_names):
        h.append("#define SONG_%s %d" % (re.sub(r"\W", "_", n), i))
    h.append("#define SONG_TITLE SONG_MENUSNG")
    h.append("#define SONG_COUNT %d" % len(song_names))
    h.append("#define MUSIC_NOTES %d" % len(psgmusic.PERIODS))
    h.append("#define FONT_CHARS \"%s\"" % FONT_CHARS.replace("\\", "\\\\").replace('"', '\\"'))
    h.append("#define FONT_COUNT %d" % len(FONT_CHARS))
    h.extend(fe["header"])
    h.append("""
typedef struct {
    unsigned int tile;      /* first tile of the right-facing frames (episode pool) */
    unsigned int tileLeft;  /* first tile of the left-facing frames */
    unsigned char frames;   /* 0 = no art */
    unsigned char dims;     /* tiles wide (low nibble) | tiles high << 4, halved */
    unsigned char cellW;    /* original cell size / 2 = on-screen size, px */
    unsigned char cellH;
} GGSprite;

typedef struct {
    unsigned char miscBank, cellBank;
    unsigned int cellAddr;
    unsigned char evBank;
    unsigned int evAddr;
    unsigned char world, episode;
    unsigned char startX, startY;
    unsigned char bossPresent, bossX, bossY;
    unsigned int items;
    unsigned char hudBright, hudDark;
    unsigned char song, bonus, planet, cycleMask, clearScene;
    unsigned int spritePal[16];
} GGStageInfo;

typedef struct {
    unsigned char slot, count, bit, step;
    signed char dir;
    unsigned int colors[4];
} GGCycle;

typedef struct {
    unsigned int tileBase;
    unsigned int tileCount;
    unsigned int bgPal[16];
    unsigned char cycleCount;
    GGCycle cycles[4];
} GGWorldInfo;

typedef struct {
    unsigned char number;           /* 1..10 as in the original's list */
    unsigned char firstStage, stages;
    unsigned int sprBase;           /* first tile of the episode's sprite pool */
    const char *title;
    const char *worlds;             /* world names, space separated */
} GGEpisodeInfo;

typedef struct { unsigned char bank; unsigned int addr; } GGSong;

extern const GGStageInfo gg_stage_info[GG_STAGES];
extern const GGWorldInfo gg_world_info[GG_WORLDS];
extern const GGEpisodeInfo gg_episode_info[GG_EPISODES];
extern const unsigned char gg_font[FONT_COUNT * 8];
extern const unsigned char gg_font_map[96];   /* ASCII 32..127 -> glyph */
extern const GGSong gg_song[SONG_COUNT];
extern const unsigned int gg_note_period[MUSIC_NOTES];
#endif""")
    open(os.path.join(OUT, "gg_data.h"), "w").write("\n".join(h) + "\n")

    c = ["/* Generated by tools/convert.py - do not edit. */", '#include "gg_data.h"', ""]
    c.append("const GGWorldInfo gg_world_info[GG_WORLDS] = {")
    for wn in world_order:
        w = worlds[wn]
        cyc = w["cycles"][:4]
        cs = ", ".join("{ %d, %d, %d, %d, %d, { %s } }" % (a, b, bit, st, d, ", ".join("0x%03X" % v for v in cols))
                       for a, b, bit, st, d, cols in cyc) or "{ 0 }"
        c.append("    { %d, %d, { %s }, %d, { %s } },  /* world %03d */" % (
            w["base"], len(w["tiles"]), ", ".join("0x%03X" % v for v in w["pal"]), len(cyc), cs, wn))
    c.append("};\n")
    c.append("const GGStageInfo gg_stage_info[GG_STAGES] = {")
    for f in stage_list:
        bp, bx, by = (1, f["boss"][0], f["boss"][1]) if f["boss"] else (0, 0, 0)
        c.append("    { %d, %d, 0x%04X, %d, 0x%04X, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, 0x%02X, %d, { %s } },  /* %s */" % (
            f["miscBank"], f["cellBank"], f["cellAddr"], f["evBank"], f["evAddr"], f["world"], f["episode"],
            f["startX"], f["startY"], bp, bx, by, f["items"], f["bright"], f["dark"],
            f["song"], f["bonus"], f["planet"], f["cycles"], f["clear"],
            ", ".join("0x%03X" % v for v in f["spal"]), f["level"]))
    c.append("};\n")
    c.append("const GGEpisodeInfo gg_episode_info[GG_EPISODES] = {")
    for e in ep_info:
        c.append('    { %d, %d, %d, %d, "%s", "%s" },' % (e["number"], e["first"], e["count"], e["sprBase"],
                                                         e["title"], e["worlds"]))
    c.append("};\n")
    c.append("const unsigned char gg_font[FONT_COUNT * 8] = {")
    for i in range(0, len(font), 16):
        c.append("    " + ", ".join("0x%02X" % v for v in font[i:i + 16]) + ",")
    c.append("};")
    fmap = []
    for code in range(32, 128):
        ch = chr(code).upper()
        fmap.append(FONT_CHARS.index(ch) if ch in FONT_CHARS else 0)
    c.append("const unsigned char gg_font_map[96] = { %s };" % ", ".join(map(str, fmap)))
    c.append("const GGSong gg_song[SONG_COUNT] = { %s };" % ", ".join("{ %d, 0x%04X }" % s for s in songs))
    c.append("const unsigned int gg_note_period[MUSIC_NOTES] = { %s };" % ", ".join(
        str(p) for p in psgmusic.PERIODS))
    c.extend(fe["fixed"])
    open(os.path.join(OUT, "gg_fixed.c"), "w").write("\n".join(c) + "\n")
    for n, key in ((3, "bank3"), (4, "bank4")):
        open(os.path.join(OUT, "gg_bank%d.c" % n), "w").write(
            "/* Generated by tools/convert.py - do not edit.  Lives in code bank %d. */\n"
            '#include "gg_data.h"\n\n' % n + "\n".join(fe[key]) + "\n")
    print("data banks: %d (%d KB), banks %d..%d; ROM %d KB" % (
        len(rom.banks), len(blob) // 1024, DATA_BANK0, DATA_BANK0 + len(rom.banks) - 1,
        total_banks * 16))


if __name__ == "__main__":
    main()
