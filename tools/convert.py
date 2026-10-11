#!/usr/bin/env python3
"""Convert the Genesis port's JJ1 data into Game Gear banks.

Everything graphical is downscaled by exactly half:
  * background: each 32x32 JJ1 block becomes a 16x16 block (2x2 GG tiles);
    the halved 8x8 tiles are deduplicated per world (with H/V flips) so the
    ROM streams them through a 256-tile VRAM cache;
  * sprites: every frame is halved too; Jazz gets pre-mirrored left frames
    because the SMS/GG VDP cannot flip sprites.

Game logic keeps the original 32 px block coordinates (the physics model is
unchanged); only the renderer divides by two.  Collision is baked here into a
per-level "cell id" map so the Z80 does one lookup per probe: each cell id
names a (render block, solid mask, feet-only mask) combination, with one-way
platforms and bridge decks folded in exactly as jj1_runtime.c computes them.

Output (gen/):
  data.bin      raw 16 KB banks starting at bank DATA_BANK0
  gg_data.h     bank numbers / offsets / table declarations
  gg_fixed.c    small tables that live in the fixed ROM
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jjdata as J

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "gen")
DATA_BANK0 = int(os.environ.get("DATA_BANK0", "4"))
BANK = 0x4000

# Event classes (jj1_events.h)
C_NONE, C_ITEM, C_WALK, C_FLY, C_HAZARD, C_SPRING, C_ONEWAY, C_END, C_DESTRUCT, \
    C_TUBE, C_BRIDGE, C_UNBOARD, C_BOSS = range(13)

N_STAGES = 7


# ----------------------------------------------------------------- colour
def gg_color(genesis):
    r, g, b = J.genesis_color_to_rgb(genesis)
    c = lambda v: (v * 15 + 3) // 7
    return c(r) | (c(g) << 4) | (c(b) << 8)


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
    """row-major list of 8x8 tiles"""
    tiles = []
    for ty in range(th):
        for tx in range(tw):
            tiles.append([img[ty * 8 + y][tx * 8:tx * 8 + 8] for y in range(8)])
    return tiles


def mirror(img):
    return [row[::-1] for row in img]


# ----------------------------------------------------------------- banks
class Banks:
    def __init__(self):
        self.banks = []

    def new(self):
        self.banks.append(bytearray(BANK))
        return DATA_BANK0 + len(self.banks) - 1

    def put(self, bank, offset, data):
        b = self.banks[bank - DATA_BANK0]
        assert offset + len(data) <= BANK, (bank, offset, len(data))
        b[offset:offset + len(data)] = data

    def blob(self):
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
}
FONT_CHARS = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-!:/.,<>'*#"


def font_tile(ch, fg, shadow):
    t = [[0] * 8 for _ in range(8)]
    rows = FONT.get(ch)
    if rows:
        for y, r in enumerate(rows):
            for x, c in enumerate(r):
                if c == '1':
                    t[y][x + 1] = fg
                    if shadow is not None:
                        if t[y + 1][x + 2] == 0:
                            t[y + 1][x + 2] = shadow
        # make sure the shadow never overwrote the glyph
        for y, r in enumerate(rows):
            for x, c in enumerate(r):
                if c == '1':
                    t[y][x + 1] = fg
    return t


# ----------------------------------------------------------------- worlds
def convert_world(world, banks, header):
    W = J.load("jj1_world%03d_tiles.inc" % world)
    words = W["jj1_world%03d_block_tiles" % world]
    gpal = W["jj1_world%03d_palette" % world]
    pal_rgb = [J.genesis_color_to_rgb(c) for c in gpal]
    nblk = len(words) // 128
    pool = {}       # canonical tile -> id
    pool_list = []
    block_refs = []
    # tile 0 of the pool is the empty tile so block 0 / missing blocks are sky
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
                    (tuple(tuple(r[::-1]) for r in t), 1),         # stored = H-flipped view
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
                # tid in bits 0-12; H flip in bit 13, V flip in bit 14, so the
                # nametable flip bits are ((ref >> 8) >> 4) & 6 (byte rotates)
                assert tid < 0x2000
                refs.append(tid | ((f & 1) << 13) | ((f >> 1) << 14))
        block_refs.append(refs)
    ntiles = len(pool_list)
    data = b"".join(tile_bytes(t) for t in pool_list)
    first = None
    for off in range(0, len(data), BANK):
        bk = banks.new()
        if first is None:
            first = bk
        banks.put(bk, 0, data[off:off + BANK])
    # palette: entry 0 is the sky (transparent on the Genesis)
    gg = [gg_color(c) for c in gpal]
    gg[0] = 0x0F44 & 0x0FFF
    print("world %d: %d blocks, %d unique halved tiles, tile bank %d" % (world, nblk, ntiles, first))
    return dict(tileBank=first, ntiles=ntiles, pal=gg, block_refs=block_refs, nblk=nblk)


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
            info = es[events[(blockY << 8) + scan]] if events[(blockY << 8) + scan] < 128 else (0, 0, 0, 0)
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
            # quick: does any bridge live within 8 cells to the left on this row?
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


def frames_of(tiles, desc, pal_rgb, flipped=False):
    """Genesis sprite descriptor -> list of halved, padded frames."""
    tile, frames, tw, th, tleft = desc
    out = []
    for f in range(frames):
        img = J.sprite_cell_image(tiles, tile + f * tw * th, tw, th)
        if flipped:
            img = mirror(img)
        hi = half(img, pal_rgb)
        hi, htw, hth = pad_to_tiles(hi)
        out.append((hi, htw, hth))
    return out


class SpritePool:
    def __init__(self):
        self.tiles = []

    def add_frames(self, frames):
        base = len(self.tiles)
        for img, tw, th in frames:
            self.tiles.extend(split_tiles(img, tw, th))
        return base


def sprite_desc(pool, gtiles, desc, pal_rgb, mirror_left=False):
    """-> 8 bytes: tile u16, tileLeft u16, frames, (tw|th<<4), cellW/2, cellH/2"""
    if desc is None or desc[1] == 0:
        return bytes(8), (0, 0)
    tile, frames, tw, th, tleft = desc
    fr = frames_of(gtiles, desc, pal_rgb)
    htw, hth = fr[0][1], fr[0][2]
    base = pool.add_frames(fr)
    if mirror_left:
        left = pool.add_frames(frames_of(gtiles, desc, pal_rgb, flipped=True))
    elif tleft != tile:
        left = pool.add_frames(frames_of(gtiles, (tleft, frames, tw, th, tleft), pal_rgb))
    else:
        left = base
    b = bytes([base & 0xFF, base >> 8, left & 0xFF, left >> 8, frames,
               htw | (hth << 4), tw * 4, th * 4])
    return b, (htw, hth)


def spring_frames(words, pal_rgb):
    """Genesis spring tiles: 3 variants of a 4x2 column-major sprite."""
    out = []
    for v in range(3):
        img = J.tiles_to_image(words[v * 64:(v + 1) * 64], 4, 2, "col")
        hi = half(img, pal_rgb)
        hi, tw, th = pad_to_tiles(hi)
        out.append((hi, tw, th))
    return out


def convert_level(stage, worlds, banks, fixed):
    world = J.STAGE_WORLD[stage]
    W = worlds[world]
    cellmap, combos, events, es, D = bake_cells(stage, W["nblk"])
    V = J.load("jj1_level%d_visuals.inc" % stage)
    P = "jj1_level%d_" % stage
    gpal1 = V[P + "pal1"]
    pal_rgb = [J.genesis_color_to_rgb(c) for c in gpal1]

    cellBank = banks.new()
    banks.put(cellBank, 0, bytes(cellmap))
    eventBank = banks.new()
    banks.put(eventBank, 0, events)
    miscBank = banks.new()

    # --- misc bank tables
    cellBlock = bytes(c[0] for c in combos) + bytes(256 - len(combos))
    cellSolid = b"".join(bytes(c[1]) for c in combos) + bytes(8 * (256 - len(combos)))
    cellDown = b"".join(bytes(c[2]) for c in combos) + bytes(8 * (256 - len(combos)))
    einfo = b"".join(bytes(e) for e in es)
    brefs = b"".join(r.to_bytes(2, "little") for b in W["block_refs"] for r in b)
    banks.put(miscBank, 0x0000, cellBlock)
    banks.put(miscBank, 0x0100, cellSolid)
    banks.put(miscBank, 0x0900, cellDown)
    banks.put(miscBank, 0x1100, einfo)
    banks.put(miscBank, 0x1300, brefs)
    # nametable high bytes (H/V flip bits) per block quadrant
    bflips = bytes(((r >> 12) & 6) for b in W["block_refs"] for r in b)
    assert len(bflips) <= 0x400
    banks.put(miscBank, 0x2200, bflips)

    # --- sprites
    pool = SpritePool()
    gtiles = V[P + "event_sprite_tiles"]
    evsprites = V[P + "event_sprites"]
    descs = bytearray()
    maxdim = {}
    for i in range(128):
        d = evsprites.get(i)
        b, dim = sprite_desc(pool, gtiles, d, pal_rgb)
        descs += b
        if d and d[1]:
            maxdim[i] = dim
    banks.put(miscBank, 0x1B00, bytes(descs))

    ptiles = V[P + "player_tiles"]
    pstates = V[P + "player_states"]
    pdesc = bytearray()
    pmax = 0
    for s in range(12):
        d = pstates.get(s)
        b, dim = sprite_desc(pool, ptiles, d, pal_rgb, mirror_left=True)
        pdesc += b
        pmax = max(pmax, dim[0] * dim[1])
    banks.put(miscBank, 0x1F00, bytes(pdesc))

    specials = bytearray()
    names = ["bird_sprite", "bird_left_sprite", "shield_sprite", "board_sprite",
             "board_left_sprite", "boss_cannon_sprite", "boss_cannon_left_sprite",
             "boss_late_sprite", "boss_late_left_sprite", "boss_spike_sprite",
             "boss_spike_left_sprite", "boss_shot_sprite"]
    for n in names:
        d = V.get(P + n)
        b, dim = sprite_desc(pool, gtiles, d, pal_rgb)
        specials += b
        maxdim[n] = dim
    # springs: 3 variants, one frame each
    sw = V.get(P + "spring_tiles")
    sbase = pool.add_frames(spring_frames(sw, pal_rgb))
    for v in range(3):
        t = sbase + v * 2
        specials += bytes([t & 0xFF, t >> 8, t & 0xFF, t >> 8, 1, 2 | (1 << 4), 16, 8])
    # boss accessory offsets (original pixels; renderer halves them)
    offs = bytearray()
    for n in ["boss_cannon_x", "boss_cannon_y", "boss_cannon_left_x", "boss_cannon_left_y",
              "boss_spike_x", "boss_spike_y", "boss_spike_left_x", "boss_spike_left_y"]:
        offs += (V.get(P + n, 0) & 0xFFFF).to_bytes(2, "little")
    assert 0x1F60 + len(specials) <= 0x1FF0, len(specials)
    banks.put(miscBank, 0x1FF0, bytes(offs))

    # stand-in tiles, halved from the Genesis gfx.c originals (sprite palette)
    def rows_img(rows):
        return [[int(ch, 16) for ch in r] for r in rows]
    gem = rows_img(["00000000", "000AA000", "00A88A00", "0A8118A0",
                    "0A8118A0", "00A88A00", "000AA000", "00000000"])
    bullet = rows_img(["00000000", "00000000", "00033000", "00311300",
                       "00311300", "00033000", "00000000", "00000000"])
    bar = [[3] * 8 for _ in range(8)]
    extra = []
    for img in (gem, bullet):
        hi, _, _ = pad_to_tiles(half(img, pal_rgb))
        # centre the 4x4 result in the 8x8 tile
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
    banks.put(miscBank, 0x1F60, bytes(specials))
    # Shared HUD font, drawn in the sprite palette's brightest/darkest entries.
    bright = max(range(1, 16), key=lambda i: sum(pal_rgb[i]))
    dark = min(range(1, 16), key=lambda i: sum(pal_rgb[i]))

    ntiles = len(pool.tiles)
    empty = bytearray((ntiles + 7) // 8)
    for i, t in enumerate(pool.tiles):
        if not any(any(r) for r in t):
            empty[i >> 3] |= 1 << (i & 7)
    assert len(empty) <= 0x200
    banks.put(miscBank, 0x2000, bytes(empty))
    data = b"".join(tile_bytes(t) for t in pool.tiles)
    sprBank = None
    for off in range(0, len(data), BANK):
        bk = banks.new()
        if sprBank is None:
            sprBank = bk
        banks.put(bk, 0, data[off:off + BANK])

    # level facts precomputed so the Z80 never scans the whole grid
    items = 0
    boss = None
    for i, e in enumerate(events):
        k = es[e][0]
        if k == C_ITEM:
            items += 1
        if k == C_BOSS and boss is None:
            boss = (i & 255, i >> 8)
    big = max(d[0] * d[1] for d in maxdim.values()) if maxdim else 0
    print("stage %d: %d cell combos, %d sprite tiles (banks %d..), items %d, boss %s, "
          "player slot %d, largest event sprite %d tiles" %
          (stage, len(combos), ntiles, sprBank, items, boss, pmax, big))
    fixed.append(dict(cellBank=cellBank, eventBank=eventBank, miscBank=miscBank,
                      sprBank=sprBank, world=world,
                      startX=D[P + "start_x"], startY=D[P + "start_y"],
                      items=items, boss=boss,
                      spal=[gg_color(c) for c in gpal1],
                      bright=bright, dark=dark))


# Music: the original MS-DOS songs (Epic's PSM modules) arranged for the PSG
# by tools/psgmusic.py, one bank per song.  JJ1_DATA points at a directory
# with the shareware's *.PSM files; without it the ROM is built silent.
JJ1_DATA = os.environ.get("JJ1_DATA", os.path.expanduser("~/jj1data"))
SONGS = [("TITLE", "MENUSNG", True), ("DIAMONDUS", "SONG0", True), ("TUBELECTRIC", "SONG3", True),
         ("MEDIVO", "SONG17", True), ("BOSS", "BOSS", True), ("ENDLEVEL", "ENDLEVEL", False)]
WORLD_SONG = [1, 2, 3]


def convert_music(banks, fixed):
    import psgmusic
    out = []
    for name, fn, loop in SONGS:
        path = os.path.join(JJ1_DATA, fn + ".PSM")
        if not os.path.exists(path):
            print("music: %s missing, song %s silent" % (path, name))
            out.append(0)
            continue
        blob, info = psgmusic.song_blob(path, loop=loop)
        assert len(blob) <= BANK, (name, len(blob))
        bk = banks.new()
        banks.put(bk, 0, blob)
        out.append(bk)
        print("music: %-12s %5d bytes, %d orders (%d unique), %d envelopes" % (
            name, len(blob), info["orders"], info["unique"], info["envs"]))
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    banks = Banks()
    worlds = [convert_world(w, banks, None) for w in range(3)]
    fixed = []
    for s in range(N_STAGES):
        convert_level(s, worlds, banks, fixed)

    songs = convert_music(banks, fixed)
    blob = banks.blob()
    open(os.path.join(OUT, "data.bin"), "wb").write(blob)

    # font as 1bpp rows (8 bytes per glyph), recoloured at load time
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
    h.append("#define DATA_BANKS %d" % len(banks.banks))
    h.append("#define GG_STAGES %d" % N_STAGES)
    h.append("#define MISC_CELLBLOCK   ((const unsigned char *)0x8000)")
    h.append("#define MISC_CELLSOLID   ((const unsigned char *)0x8100)")
    h.append("#define MISC_CELLDOWN    ((const unsigned char *)0x8900)")
    h.append("#define MISC_EVENTINFO   ((const unsigned char *)0x9100)")
    h.append("#define MISC_BLOCKREFS   ((const unsigned int *)0x9300)")
    h.append("#define MISC_EVSPRITES   ((const GGSprite *)0x9B00)")
    h.append("#define MISC_PLAYER      ((const GGSprite *)0x9F00)")
    h.append("#define MISC_SPECIALS    ((const GGSprite *)0x9F60)")
    h.append("#define MISC_BOSSOFFS    ((const signed int *)0x9FF0)")
    h.append("#define MISC_SPREMPTY    ((const unsigned char *)0xA000)")
    h.append("#define MISC_BLOCKFLIPS  ((const unsigned char *)0xA200)")
    h.append("#define TILE_POOL_MAX %d" % max(w["ntiles"] for w in worlds))
    for i, n in enumerate(["BIRD", "BIRD_LEFT", "SHIELD", "BOARD", "BOARD_LEFT",
                           "BOSS_CANNON", "BOSS_CANNON_LEFT", "BOSS_LATE", "BOSS_LATE_LEFT",
                           "BOSS_SPIKE", "BOSS_SPIKE_LEFT", "BOSS_SHOT",
                           "SPRING0", "SPRING1", "SPRING2", "GEM", "BULLET", "BOSSBAR"]):
        h.append("#define SPECIAL_%s %d" % (n, i))
    for i, (n, fn, loop) in enumerate(SONGS):
        h.append("#define SONG_%s %d" % (n, i))
    h.append("#define SONG_COUNT %d" % len(SONGS))
    import psgmusic
    h.append("#define MUSIC_NOTES %d" % len(psgmusic.PERIODS))
    h.append("#define FONT_CHARS \"%s\"" % FONT_CHARS.replace("'", "\\'"))
    h.append("#define FONT_COUNT %d" % len(FONT_CHARS))
    h.append("""
typedef struct {
    unsigned int tile;      /* first tile of the right-facing frames */
    unsigned int tileLeft;  /* first tile of the left-facing frames */
    unsigned char frames;   /* 0 = no art */
    unsigned char dims;     /* tiles wide (low nibble) | tiles high << 4, halved */
    unsigned char cellW;    /* original cell size / 2 = on-screen size, px */
    unsigned char cellH;
} GGSprite;

typedef struct {
    unsigned char cellBank, eventBank, miscBank, sprBank, world;
    unsigned char startX, startY;
    unsigned char bossPresent, bossX, bossY;
    unsigned int items;
    unsigned char hudBright, hudDark;
    unsigned int spritePal[16];
} GGStageInfo;

typedef struct {
    unsigned char tileBank;
    unsigned int tileCount;
    unsigned int bgPal[16];
} GGWorldInfo;

extern const GGStageInfo gg_stage_info[GG_STAGES];
extern const GGWorldInfo gg_world_info[3];
extern const unsigned char gg_font[FONT_COUNT * 8];
extern const unsigned char gg_song_bank[SONG_COUNT];
extern const unsigned char gg_stage_song[GG_STAGES];
extern const unsigned int gg_note_period[MUSIC_NOTES];
#endif""")
    open(os.path.join(OUT, "gg_data.h"), "w").write("\n".join(h) + "\n")

    c = ["/* Generated by tools/convert.py - do not edit. */", '#include "gg_data.h"', ""]
    c.append("const GGWorldInfo gg_world_info[3] = {")
    for w in worlds:
        c.append("    { %d, %d, { %s } }," % (w["tileBank"], w["ntiles"],
                                             ", ".join("0x%03X" % v for v in w["pal"])))
    c.append("};\n")
    c.append("const GGStageInfo gg_stage_info[GG_STAGES] = {")
    for f in fixed:
        bp, bx, by = (1, f["boss"][0], f["boss"][1]) if f["boss"] else (0, 0, 0)
        c.append("    { %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, { %s } }," % (
            f["cellBank"], f["eventBank"], f["miscBank"], f["sprBank"], f["world"],
            f["startX"], f["startY"], bp, bx, by, f["items"], f["bright"], f["dark"],
            ", ".join("0x%03X" % v for v in f["spal"])))
    c.append("};\n")
    c.append("const unsigned char gg_font[FONT_COUNT * 8] = {")
    for i in range(0, len(font), 16):
        c.append("    " + ", ".join("0x%02X" % v for v in font[i:i + 16]) + ",")
    c.append("};")
    c.append("const unsigned char gg_song_bank[SONG_COUNT] = { %s };" % ", ".join(str(b) for b in songs))
    c.append("const unsigned char gg_stage_song[GG_STAGES] = { %s };" % ", ".join(
        str(4 if f["boss"] else WORLD_SONG[f["world"]]) for f in fixed))
    c.append("const unsigned int gg_note_period[MUSIC_NOTES] = { %s };" % ", ".join(
        str(p) for p in psgmusic.PERIODS))
    open(os.path.join(OUT, "gg_fixed.c"), "w").write("\n".join(c) + "\n")
    print("data banks: %d (%d KB), banks %d..%d" % (len(banks.banks), len(blob) // 1024,
                                                    DATA_BANK0, DATA_BANK0 + len(banks.banks) - 1))


if __name__ == "__main__":
    main()
