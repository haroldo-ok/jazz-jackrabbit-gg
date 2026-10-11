"""Loader for the Genesis port's generated JJ1 data includes.

The Genesis repository ships everything as C arrays (.inc files).  This module
parses those arrays back into Python so the Game Gear converter can re-encode
them: 4bpp Genesis patterns -> pixel images -> half-size GG tiles.
"""
import os
import re

SRC = os.environ.get("JJ_GENESIS_SRC", os.path.expanduser("~/jj/src"))

_ARRAY = re.compile(
    r"const\s+(u8|u16|u32|s16|Jj1EventSprite|Jj1EventInfo)\s+(\w+)\s*(\[[^\]]*\])?\s*=\s*(\{.*?\})\s*;",
    re.S)
_SCALAR = re.compile(r"const\s+(u16|s16)\s+(\w+)\s*=\s*(-?\w+)\s*;")


def _rgb333(m):
    r, g, b = (int(x) for x in m.groups())
    return str((b << 9) | (g << 5) | (r << 1))


def _strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"RGB3_3_3_TO_VDPCOLOR\((\d+),\s*(\d+),\s*(\d+)\)", _rgb333, text)
    return text


def _parse_struct_list(body, fields):
    """{ [n] = { a, b, ... }, ... } or { {..}, {..} } -> dict index->tuple."""
    inner = body.strip()[1:-1]
    out = {}
    idx = 0
    for m in re.finditer(r"(?:\[(\d+)\]\s*=\s*)?\{([^{}]*)\}", inner):
        if m.group(1) is not None:
            idx = int(m.group(1))
        vals = tuple(int(v.strip(), 0) for v in m.group(2).split(",") if v.strip())
        out[idx] = vals
        idx += 1
    return out


def parse_file(path):
    text = _strip(open(path).read())
    syms = {}
    for m in _ARRAY.finditer(text):
        typ, name, _dim, body = m.groups()
        if typ in ("Jj1EventSprite", "Jj1EventInfo"):
            if _dim is None:  # single struct
                vals = tuple(int(v.strip(), 0) for v in body.strip()[1:-1].split(",") if v.strip())
                syms[name] = vals
            else:
                syms[name] = _parse_struct_list(body, None)
        else:
            vals = [int(v.strip(), 0) for v in body.strip()[1:-1].split(",") if v.strip()]
            syms[name] = vals
    for m in _SCALAR.finditer(text):
        typ, name, val = m.groups()
        syms[name] = int(val, 0)
    return syms


_cache = {}


def load(name):
    if name not in _cache:
        _cache[name] = parse_file(os.path.join(SRC, name))
    return _cache[name]


def eventset(level):
    """Ground-truth event table: id -> (klass, param, points, strength)."""
    text = _strip(open(os.path.join(SRC, "jj1_level%d_eventset.inc" % level)).read())
    m = re.search(r"jj1_level%d_eventset\[128\]\s*=\s*(\{.*?\});" % level, text, re.S)
    table = _parse_struct_list(m.group(1), None)
    return [table.get(i, (0, 0, 0, 0)) for i in range(128)]


# Episode layout (from jj1_episode_stages.inc): stage -> world.
STAGE_WORLD = [0, 0, 1, 1, 2, 2, 2]


def genesis_color_to_rgb(c):
    """Genesis 0BBB0GGG0RRR0 -> (r, g, b) in 0..7."""
    return ((c >> 1) & 7, (c >> 5) & 7, (c >> 9) & 7)


def decode_tile(words):
    """8 u32 rows (one nibble per pixel, MSB first) -> 8x8 list of indices."""
    out = []
    for w in words:
        out.append([(w >> (28 - 4 * i)) & 15 for i in range(8)])
    return out


def tiles_to_image(tile_words, tw, th, order="row"):
    """Assemble tw x th tiles (8 words each) into a pixel grid.
    order='row': row-major; 'col': column-major (Genesis sprite order)."""
    img = [[0] * (tw * 8) for _ in range(th * 8)]
    for t in range(tw * th):
        if order == "row":
            tx, ty = t % tw, t // tw
        else:
            tx, ty = t // th, t % th
        tile = decode_tile(tile_words[t * 8:(t + 1) * 8])
        for y in range(8):
            img[ty * 8 + y][tx * 8:tx * 8 + 8] = tile[y]
    return img


def sprite_cell_image(tiles, base, tilesW, tilesH):
    """Rebuild one sprite cell exactly as main.c draws it: the cell is split
    into chunks of at most 4x4 tiles (row of chunks, then next row), each chunk
    stored column-major, chunks consecutive in tile memory."""
    img = [[0] * (tilesW * 8) for _ in range(tilesH * 8)]
    tile = base
    for cy in range(0, tilesH, 4):
        ch = min(4, tilesH - cy)
        for cx in range(0, tilesW, 4):
            cw = min(4, tilesW - cx)
            words = tiles[tile * 8:(tile + cw * ch) * 8]
            chunk = tiles_to_image(words, cw, ch, order="col")
            for y in range(ch * 8):
                img[cy * 8 + y][cx * 8:cx * 8 + cw * 8] = chunk[y]
            tile += cw * ch
    return img
