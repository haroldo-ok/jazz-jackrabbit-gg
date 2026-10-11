import sys, itertools
sys.path.insert(0, __import__("os").path.dirname(__file__))
import jjdata as J

def half(img, pal_rgb):
    h, w = len(img) // 2, len(img[0]) // 2
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            px = [img[2*y][2*x], img[2*y][2*x+1], img[2*y+1][2*x], img[2*y+1][2*x+1]]
            solid = [p for p in px if p]
            if len(solid) < 2:
                out[y][x] = 0; continue
            # mode if a majority colour exists, else nearest to average
            best = max(set(solid), key=solid.count)
            if solid.count(best) >= 2:
                out[y][x] = best; continue
            r = sum(pal_rgb[p][0] for p in solid) / len(solid)
            g = sum(pal_rgb[p][1] for p in solid) / len(solid)
            b = sum(pal_rgb[p][2] for p in solid) / len(solid)
            out[y][x] = min(range(1, 16), key=lambda i: (pal_rgb[i][0]-r)**2+(pal_rgb[i][1]-g)**2+(pal_rgb[i][2]-b)**2)
    return out

def tkey(t):
    a = tuple(tuple(r) for r in t)
    hf = tuple(tuple(r[::-1]) for r in t)
    vf = a[::-1]
    hv = hf[::-1]
    return min(a, hf, vf, hv)

for world in range(3):
    W = J.load("jj1_world%03d_tiles.inc" % world)
    tiles = W["jj1_world%03d_block_tiles" % world]
    pal = [J.genesis_color_to_rgb(c) for c in W["jj1_world%03d_palette" % world]]
    blocks = []
    nblk = len(tiles) // 128
    for b in range(nblk):
        img = J.tiles_to_image(tiles[b*128:(b+1)*128], 4, 4, "row")
        hb = half(img, pal)
        ts = []
        for ty in range(2):
            for tx in range(2):
                ts.append(tkey([row[tx*8:tx*8+8] for row in hb[ty*8:ty*8+8]]))
        blocks.append(ts)
    alltiles = set(t for b in blocks for t in b)
    print("world", world, "unique halved tiles (flip-dedup) for all 240 blocks:", len(alltiles))
    for stage, w in enumerate(J.STAGE_WORLD):
        if w != world: continue
        D = J.load("jj1_level%d_data.inc" % stage)
        m = D["jj1_level%d_blocks" % stage]
        used = set(m)
        ut = set(t for b in used if b < nblk for t in blocks[b])
        worstB = worstT = 0
        for oy in range(0, 64 - 10 + 1):
            for ox in range(0, 256 - 11 + 1, 1):
                win = set(m[(oy+y)*256 + ox + x] for y in range(10) for x in range(11))
                if len(win) > worstB: worstB = len(win)
                if len(win) > 40:
                    tt = set(t for b in win if b < nblk for t in blocks[b])
                    worstT = max(worstT, len(tt))
        print(" stage", stage, "blocks used", len(used), "tiles used", len(ut), "worst window blocks", worstB, "tiles", worstT)
