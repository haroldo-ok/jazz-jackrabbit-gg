/* Game Gear renderer for the Jazz Jackrabbit port.
 *
 * World coordinates are original JJ1 pixels; the screen shows them at HALF
 * size, so the 160x144 Game Gear screen sees a 320x288 slice of the level
 * (about what the original showed) and each 32 px JJ1 block is a 16x16
 * block of 2x2 tiles.
 *
 * Background: the halved block art is a per-world pool of up to ~600 unique
 * 8x8 tiles, far more than VRAM holds, so tiles stream through a 255-slot
 * cache.  Each slot is reference counted by the cells of the 11x10-block
 * window that covers the screen; a slot is only reused once nothing in the
 * window shows it, so visible tiles are never overwritten.  The window moves
 * one block row/column at a time over a 16x14-block ring in the 32x28
 * nametable.
 *
 * Sprites: original frames halved by the converter.  Jazz streams into one
 * slot; enemy and item frames go through small LRU caches keyed by the
 * frame's first pool tile, so a looping walk cycle (and several enemies of
 * one kind) stop costing VRAM uploads once warm.  The HUD is drawn with
 * sprites because the VDP has a single background layer. */

#include <SMSlib.h>
#include <string.h>
#include "jazz_game.h"
#include "gg_data.h"
#include "render.h"

extern u8 gg_cellBank, gg_eventBank, gg_miscBank, gg_sprBank, gg_worldNo;

/* ------------------------------------------------------------ VRAM plan */
/* Sprite tile indices (VRAM tiles 256 + n). */
#define SPR_FONT     0     /* 25 HUD glyphs, see HUD_GLYPHS */
#define SPR_PLAYER   25    /* 6: halved 4x6 cell = 2x3 */
#define SPR_BOARD    31    /* 4 */
#define SPR_ENEMY    35    /* 9 blocks x 12 */
#define ENEMY_BLOCKS 9
#define ENEMY_BLOCK_TILES 12
#define SPR_ITEM     143   /* 8 blocks x 4 (top two: shield, bird) */
#define ITEM_BLOCKS  8
#define ITEM_CACHE   6
#define ITEM_BLOCK_TILES 4
#define SPR_SPRING   175   /* 3 x 2 */
#define SPR_GEM      181
#define SPR_BULLET   182
#define SPR_BOSSBAR  183

/* The in-game font holds only what the HUD prints. */
static const char HUD_GLYPHS[] = "0123456789*#BTMOSPAUEDWLN";
#define G_HEART 10
#define G_FACE  11

#define GG_X0 48
#define GG_Y0 24

#define WIN_W 11
#define WIN_H 10

s16 camX, camY;
static s16 winX = -100, winY = -100;
static u8 scrollX, scrollY;
u8 cacheMisses;   /* per frame, for profiling/tests */
u8 spriteUploads; /* per frame */

/* ------------------------------------------------------------ tile cache */
/* Reference counting is per render block: a block in use by some window
 * cell holds one reference on each of its four tiles' cache slots, and its
 * slots are remembered in blockSlot[] while blockRef[] > 0.  Streaming a
 * cell in or out then only touches the tile cache when the first/last copy
 * of a block enters/leaves the window.  The three tables sit at fixed,
 * page-aligned addresses above the C data so the hand-written loops index
 * them with one register load (the Makefile checks the data stays below). */
u8 tileSlot[TILE_POOL_MAX];
static u16 slotTile[256];
u8 __at(0xD700) blockRef[256];
u8 __at(0xD800) blockSlot[1024];
u8 __at(0xDC00) slotRef[256];
static u8 nextSlot;
static u8 curTileBank;

static void cache_reset(void)
{
    u16 i;
    for (i = 0; i < TILE_POOL_MAX; i++) tileSlot[i] = 0;
    for (i = 0; i < 256; i++) { slotTile[i] = 0xFFFF; slotRef[i] = 0; blockRef[i] = 0; }
    nextSlot = 1;
}

/* Copy one 32-byte tile to VRAM address `dst`, address and data in one
 * interrupt-free burst (the sky-gradient line interrupt rewrites the VDP
 * address, so no transfer may be split by it).  HL = src, DE = dst. */
static void vram_tile(const void *src, u16 dst) __naked
{
    (void)src; (void)dst;
    __asm
    ld  a, d
    or  a, #0x40
    ld  d, a
    ld  c, #0xBF
    di
    out (c), e
    out (c), d
    ld  c, #0xBE
    ld  b, #32
09601$:
    outi
    jr  nz, 09601$
    ei
    ret
    __endasm;
}

/* Upload pool tile `tid` into a slot nothing in the window uses. */
static u8 cache_miss(u16 tid)
{
    u8 s, n;
    for (n = 255; n; n--) {
        s = nextSlot;
        nextSlot++;
        if (!nextSlot) nextSlot = 1;
        if (!slotRef[s]) break;
    }
    if (!n) return 0;   /* cache full: draw sky rather than corrupt */
    if (slotTile[s] != 0xFFFF) tileSlot[slotTile[s]] = 0;
    slotTile[s] = tid;
    tileSlot[tid] = s;
    SMS_mapROMBank(curTileBank + (u8)(tid >> 9));
    vram_tile((const void *)(0x8000 + ((tid & 511) << 5)), (u16)s << 5);
    cacheMisses++;
    return s;
}

/* First copy of block `blk` entered the window: pin its four tiles.  Called
 * with the misc bank mapped, and leaves it mapped. */
void block_acquire(u8 blk)
{
    u16 r[4];
    u16 o = blk;
    u8 q, missed = 0;
    u8 *bs;
    o <<= 2;
    memcpy(r, MISC_BLOCKREFS + o, 8);
    bs = blockSlot + o;
    for (q = 0; q < 4; q++) {
        u16 tid = r[q] & 0x1FFF;
        u8 sl = 0;
        if (tid) {
            sl = tileSlot[tid];
            if (!sl) { sl = cache_miss(tid); missed = 1; }
            if (sl) slotRef[sl]++;
        }
        bs[q] = sl;
    }
    if (missed) SMS_mapROMBank(gg_miscBank);
}

/* Last copy of block `blk` left the window: unpin its tiles. */
void block_release(u8 blk)
{
    u16 o = blk;
    u8 q, *bs;
    o <<= 2;
    bs = blockSlot + o;
    for (q = 0; q < 4; q++) {
        u8 sl = bs[q];
        if (sl && slotRef[sl]) slotRef[sl]--;
    }
}

/* Render blocks for a run of n cells starting at (bx, by), stepping right
 * (horizontal) or down.  Cells outside the map, and destroyed blocks unless
 * `original`, show block 0 (sky).  Leaves the misc bank mapped. */
#define RUN_MAX 11
u8 runBlk[RUN_MAX];

static void run_blocks(s16 bx, s16 by, u8 n, u8 horizontal, u8 original)
{
    u8 i, x, y, lo = 0, hi = 0;
    u8 *bp;
    /* Clip the run to the map: cells outside show block 0. */
    for (i = 0; i < n; i++) runBlk[i] = 0;
    if ((by < 0) || (by >= 64) || (bx < -RUN_MAX) || (bx >= 256)) goto done;
    if (horizontal) {
        u8 skip = 0, cnt = n;
        if (bx < 0) { skip = (u8)(-bx); bx = 0; cnt -= skip; }
        if (bx + cnt > 256) cnt = (u8)(256 - bx);
        x = (u8)bx;
        y = (u8)by;
        {
            u16 off = y;
            const u8 *src;
            off <<= 8;
            off |= x;
            src = (const u8 *)(0x8000 + off);
            SMS_mapROMBank(gg_cellBank);
            memcpy(runBlk + skip, src, cnt);
            lo = skip;
            hi = (u8)(skip + cnt);
        }
    } else {
        u8 cnt = n;
        if (bx < 0) goto done;
        if (by + cnt > 64) cnt = (u8)(64 - by);
        x = (u8)bx;
        y = (u8)by;
        {
            u16 off = y;
            const u8 *src;
            off <<= 8;
            off |= x;
            src = (const u8 *)(0x8000 + off);
            SMS_mapROMBank(gg_cellBank);
            bp = runBlk;
            for (i = 0; i < cnt; i++) { *bp++ = *src; src += 256; }
            lo = 0;
            hi = cnt;
        }
    }
    /* cell ids -> render blocks (destroyed blocks show the sky) */
    SMS_mapROMBank(gg_miscBank);
    for (i = lo; i < hi; i++) runBlk[i] = MISC_CELLBLOCK[runBlk[i]];
    if (!original && game.destroyedCount) {
        s16 cx = bx, cy = by;
        if (horizontal) cx -= lo;
        for (i = 0; i < n; i++) {
            if (((u16)cx < 256) && ((u16)cy < 64) && gg_cell_destroyed((u8)cx, (u8)cy)) runBlk[i] = 0;
            if (horizontal) cx++; else cy++;
        }
    }
done:
    SMS_mapROMBank(gg_miscBank);
}

/* Stream the ra_n blocks in runBlk into the window, hand-written: count each
 * block's reference (pinning its tiles on the first one) and write its 2x2
 * nametable entries at (ra_col, ra_row), stepping right or down.  Nametable
 * writes go out at >= 28 cycles a byte (VRAM safe during display), each
 * address+data group with interrupts off (the line interrupt writes CRAM). */
u8 ra_n, ra_horiz, ra_col, ra_row;
u8 *ra_bp;
u16 ra_addr;

static void run_add_asm(void) __naked
{
    __asm
    ld  hl, #_runBlk
    ld  (_ra_bp), hl
09501$:
    ld  a, (_ra_n)
    or  a, a
    ret z
    dec a
    ld  (_ra_n), a
    ld  hl, (_ra_bp)
    ld  a, (hl)
    inc hl
    ld  (_ra_bp), hl
    ld  l, a
    ld  h, #0xD7
    ld  b, (hl)
    inc (hl)
    ld  c, a
    ld  a, b
    or  a, a
    jr  nz, 09502$
    ld  a, c
    push bc
    call _block_acquire
    pop bc
09502$:
    ; ---- nametable address: 0x7800 | row << 6 | col << 1
    ld  a, (_ra_row)
    ld  l, a
    ld  h, #0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    ld  a, (_ra_col)
    add a, a
    or  a, l
    ld  l, a
    ld  a, h
    or  a, #0x78
    ld  h, a
    ld  (_ra_addr), hl
    ; ---- HL = blockSlot + blk*4, DE = flips + blk*4
    ld  l, c
    ld  h, #0
    add hl, hl
    add hl, hl
    ld  d, h
    ld  e, l
    ld  a, h
    add a, #0xD8
    ld  h, a
    ld  a, d
    add a, #0xA2
    ld  d, a
    ld  bc, (_ra_addr)
    di
    ld  a, c
    out (0xBF), a
    ld  a, b
    out (0xBF), a
    ld  a, (hl)
    inc hl
    nop
    out (0xBE), a
    ld  a, (de)
    inc de
    nop
    out (0xBE), a
    ld  a, (hl)
    inc hl
    nop
    out (0xBE), a
    ld  a, (de)
    inc de
    nop
    out (0xBE), a
    ei
    ld  a, c
    add a, #64
    ld  c, a
    jr  nc, 09503$
    inc b
09503$:
    di
    ld  a, c
    out (0xBF), a
    ld  a, b
    out (0xBF), a
    ld  a, (hl)
    inc hl
    nop
    out (0xBE), a
    ld  a, (de)
    inc de
    nop
    out (0xBE), a
    ld  a, (hl)
    nop
    nop
    nop
    out (0xBE), a
    ld  a, (de)
    nop
    nop
    nop
    out (0xBE), a
    ei
    ; ---- advance
    ld  a, (_ra_horiz)
    or  a, a
    jr  z, 09512$
    ld  a, (_ra_col)
    add a, #2
    and a, #31
    ld  (_ra_col), a
    jp  09501$
09512$:
    ld  a, (_ra_row)
    add a, #2
    cp  a, #28
    jr  c, 09513$
    xor a, a
09513$:
    ld  (_ra_row), a
    jp  09501$
    __endasm;
}

/* Drop the window's references on the ra_n blocks in runBlk. */
static void run_remove_asm(void) __naked
{
    __asm
    ld  hl, #_runBlk
    ld  a, (_ra_n)
    ld  b, a
09521$:
    ld  a, (hl)
    inc hl
    ld  e, a
    ld  d, #0xD7
    ld  a, (de)
    or  a, a
    jr  z, 09522$
    dec a
    ld  (de), a
    jr  nz, 09522$
    push hl
    push bc
    ld  a, e
    call _block_release
    pop bc
    pop hl
09522$:
    djnz 09521$
    ret
    __endasm;
}

static void run_add(s16 bx, s16 by, u8 n, u8 horizontal)
{
    run_blocks(bx, by, n, horizontal, 0);
    ra_col = (u8)((bx << 1) & 31);
    ra_row = (u8)((u8)by % 14) << 1;
    ra_n = n;
    ra_horiz = horizontal;
    run_add_asm();
}

static void run_remove(s16 bx, s16 by, u8 n, u8 horizontal, u8 original)
{
    run_blocks(bx, by, n, horizontal, original);
    ra_n = n;
    run_remove_asm();
}

/* ------------------------------------------------- items in the window */
/* Cells of interest (items, springs, bridges) inside the window, kept up to
 * date incrementally as the window moves. */
#define MAX_LIST 40
/* One record per cell of interest; px/py are the cell's position in half
 * (screen) pixels, precomputed so drawing only adds the camera base. */
typedef struct {
    u8 gx, gy, id, klass;
    s16 px, py;
    u8 aux;          /* springs: sprite tile of the variant */
    u8 pad;
} ItemEntry;
static ItemEntry items[MAX_LIST];
static u8 listCount;

static void list_add(u8 x, u8 y, u8 id)
{
    u8 k = gg_einfo[id].klass;
    ItemEntry *it;
    if (((k != JJ1_CLASS_ITEM) && (k != JJ1_CLASS_SPRING) && (k != JJ1_CLASS_BRIDGE)) ||
        (listCount >= MAX_LIST)) return;
    it = &items[listCount];
    it->gx = x;
    it->gy = y;
    it->id = id;
    it->klass = k;
    it->px = (s16)x << 4;
    it->py = (s16)y << 4;
    if (k == JJ1_CLASS_SPRING) {
        u8 p = gg_einfo[id].param;
        it->aux = (u8)(SPR_SPRING + ((p >= 12) ? 4 : ((p >= 9) ? 2 : 0)));
    }
    listCount++;
}

static void list_scan(s16 bx, s16 by, u8 n, u8 horizontal)
{
    u8 i, x, y, cnt;
    u16 off;
    const u8 *src;
    s16 c = n;
    if (horizontal) {
        if ((by < 0) || (by >= 64)) return;
        if (bx < 0) { c += bx; bx = 0; }
        if (bx + c > 256) c = 256 - bx;
    } else {
        if ((bx < 0) || (bx >= 256)) return;
        if (by < 0) { c += by; by = 0; }
        if (by + c > 64) c = 64 - by;
    }
    if (c <= 0) return;
    cnt = (u8)c;
    x = (u8)bx;
    y = (u8)by;
    off = y;
    off <<= 8;
    off |= x;
    src = (const u8 *)(0x8000 + off);
    SMS_mapROMBank(gg_eventBank);
    if (horizontal) {
        for (i = 0; i < cnt; i++, src++)
            if (*src) list_add((u8)(x + i), y, *src);
    } else {
        for (i = 0; i < cnt; i++, src += 256)
            if (*src) list_add(x, (u8)(y + i), *src);
    }
}

/* Drop entries that left the window (winX/winY already updated). */
static void list_prune(void)
{
    u8 i, j = 0;
    ItemEntry *src = items, *dst = items;
    for (i = 0; i < listCount; i++, src++) {
        s16 x = src->gx, y = src->gy;
        if ((x < winX) || (x >= winX + WIN_W) || (y < winY) || (y >= winY + WIN_H)) continue;
        if (dst != src) memcpy(dst, src, sizeof(ItemEntry));
        dst++;
        j++;
    }
    listCount = j;
}

/* ------------------------------------------------------------- window */
static void window_full(s16 nx, s16 ny)
{
    u8 y;
    u16 i;
    for (i = 0; i < 256; i++) { slotRef[i] = 0; blockRef[i] = 0; }
    winX = nx;
    winY = ny;
    listCount = 0;
    for (y = 0; y < WIN_H; y++) {
        run_add(nx, ny + y, WIN_W, 1);
        list_scan(nx, ny + y, WIN_W, 1);
    }
}

u8 winMoved;
static void window_move(s16 nx, s16 ny)
{
    s16 dx = nx - winX, dy = ny - winY;
    if (!dx && !dy) return;
    winMoved++;
    if ((dx > 1) || (dx < -1) || (dy > 1) || (dy < -1)) { window_full(nx, ny); return; }
    if (dx == 1) {
        run_remove(winX, winY, WIN_H, 0, 0);
        run_add(winX + WIN_W, winY, WIN_H, 0);
        list_scan(winX + WIN_W, winY, WIN_H, 0);
    } else if (dx == -1) {
        run_remove(winX + WIN_W - 1, winY, WIN_H, 0, 0);
        run_add(winX - 1, winY, WIN_H, 0);
        list_scan(winX - 1, winY, WIN_H, 0);
    }
    winX = nx;
    if (dy == 1) {
        run_remove(winX, winY, WIN_W, 1, 0);
        run_add(winX, winY + WIN_H, WIN_W, 1);
        list_scan(winX, winY + WIN_H, WIN_W, 1);
    } else if (dy == -1) {
        run_remove(winX, winY + WIN_H - 1, WIN_W, 1, 0);
        run_add(winX, winY - 1, WIN_W, 1);
        list_scan(winX, winY - 1, WIN_W, 1);
    }
    winY = ny;
    list_prune();
}

static u8 renderedDestroyed;

/* Replay destroyed blocks the renderer has not drawn yet. */
static void apply_destroyed(void)
{
    while (renderedDestroyed < game.destroyedCount) {
        u8 gx = game.destroyedX[renderedDestroyed];
        u8 gy = game.destroyedY[renderedDestroyed];
        renderedDestroyed++;
        if (((s16)gx >= winX) && ((s16)gx < winX + WIN_W) &&
            ((s16)gy >= winY) && ((s16)gy < winY + WIN_H)) {
            run_remove(gx, gy, 1, 1, 1);
            run_add(gx, gy, 1, 1);
        }
    }
}

/* ------------------------------------------------------------- sprites */
static u8 uploadBudget;   /* tiles that may still be streamed this frame */
static u8 hudBright, hudDark;

static void read_sprite(const GGSprite *src, GGSprite *dst)
{
    SMS_mapROMBank(gg_miscBank);
    *dst = *src;
}

/* 1 for each sprite VRAM tile that is fully transparent (halved art keeps
 * blank padding tiles; skipping them saves sprites and the per-line budget). */
u8 slotEmpty[192];

/* Copy upl_n consecutive pool tiles from upl_tile into sprite VRAM tiles
 * upl_slot.., recording blank tiles in slotEmpty[] (and skipping their
 * upload).  Hand-written: VRAM writes are paced at 28 cycles a byte, which is
 * safe during active display on every SMS/GG VDP. */
u16 upl_tile;
u8 upl_slot, upl_n;
static const u8 bitOf[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };

static void upload_asm(void) __naked
{
    __asm
09401$:
    ld  a, (_upl_n)
    or  a, a
    ret z
    dec a
    ld  (_upl_n), a
    ; --- empty bit: MISC_SPREMPTY[tile >> 3] & bitOf[tile & 7]
    ld  a, (_gg_miscBank)
    ld  (0xFFFF), a
    ld  hl, (_upl_tile)
    ld  a, l
    and a, #7
    ld  e, a
    ld  d, #0
    srl h
    rr  l
    srl h
    rr  l
    srl h
    rr  l
    ld  bc, #0xA000
    add hl, bc
    ld  c, (hl)
    ld  hl, #_bitOf
    add hl, de
    ld  a, (hl)
    and a, c
    ld  c, a
    ld  a, (_upl_slot)
    ld  e, a
    ld  hl, #_slotEmpty
    add hl, de
    ld  (hl), #0
    ld  a, c
    or  a, a
    jr  z, 09402$
    ld  (hl), #1
    jr  09403$
09402$:
    ; --- bank = sprBank + (tile >> 9); src = 0x8000 + (tile & 511) * 32
    ld  hl, (_upl_tile)
    ld  a, h
    srl a
    ld  b, a
    ld  a, (_gg_sprBank)
    add a, b
    ld  (0xFFFF), a
    ld  a, h
    and a, #1
    ld  h, a
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    set 7, h
    ; --- VRAM address 0x2000 + slot * 32, write flag 0x4000
    ld  a, (_upl_slot)
    ld  e, a
    ld  d, #0
    ex  de, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    ld  a, h
    or  a, #0x60
    ld  h, a
    ex  de, hl
    ld  c, #0xBF
    di
    out (c), e
    out (c), d
    ld  c, #0xBE
    ld  b, #32
09404$:
    outi
    jr  nz, 09404$
    ei
09403$:
    ld  hl, (_upl_tile)
    inc hl
    ld  (_upl_tile), hl
    ld  hl, #_upl_slot
    inc (hl)
    jp  09401$
    __endasm;
}

static void upload_sprite_tiles(u16 tile, u8 dstSlot, u8 n)
{
    spriteUploads += n;
    upl_tile = tile;
    upl_slot = dstSlot;
    upl_n = n;
    upload_asm();
}

/* Sprite emitter, hand-written (SDCC's version cost ~1000 cycles a tile).
 * draw_cell_asm(HL = sx, DE = sy) emits dc_dims (tw | th << 4) tiles from
 * sprite slot dc_slot upward, row-major, straight into SMSlib's sprite table
 * (SpriteTableY[n] = y - 1, SpriteTableXN[2n] = x, tile).  Each tile is
 * clipped to the Game Gear window and skipped when slotEmpty[] marks it. */
extern u8 SpriteTableY[64], SpriteTableXN[128], SpriteNextFree;
u8 dc_slot, dc_dims, dc_tw, dc_th, dc_ybyte, dc_room;
s16 dc_x0, dc_y0;

/* Visible GG window in SMS sprite coordinates: x 41..207, y 17..167 (a tile
 * whose top-left is there shows at least one pixel). */
static void draw_cell_asm(s16 sx, s16 sy) __naked
{
    (void)sx; (void)sy;
    __asm
    push ix
    push iy
    ld  (_dc_x0), hl
    ld  (_dc_y0), de
    ld  a, (_dc_dims)
    ld  c, a
    and a, #0x0F
    jp  z, 09299$
    ld  (_dc_tw), a
    ld  a, c
    rrca
    rrca
    rrca
    rrca
    and a, #0x0F
    jp  z, 09299$
    ld  (_dc_th), a
    ; sprite table write pointers from SpriteNextFree
    ld  a, (_SpriteNextFree)
    ld  e, a
    ld  d, #0
    ld  iy, #_SpriteTableY
    add iy, de
    ld  ix, #_SpriteTableXN
    add ix, de
    add ix, de
    ld  a, #64
    sub a, e
    ld  (_dc_room), a
    ; fully visible horizontally?  0 <= x0-41 and x0-41+8*tw <= 167
    ld  hl, (_dc_x0)
    ld  de, #-41
    add hl, de
    ld  a, h
    or  a, a
    jp  nz, 09250$
    ld  a, (_dc_tw)
    add a, a
    add a, a
    add a, a
    add a, l
    jp  c, 09250$
    cp  a, #168
    jp  nc, 09250$
    ; ---------------- fast path: 8-bit x
    ld  a, (_dc_th)
    ld  b, a
    ld  hl, (_dc_y0)
09201$:
    push bc
    push hl
    ld  de, #-17
    add hl, de
    ld  a, h
    or  a, a
    jr  nz, 09210$
    ld  a, l
    cp  a, #151
    jr  nc, 09210$
    add a, #16
    ld  e, a
    ld  a, (_dc_x0)
    ld  d, a
    ld  a, (_dc_slot)
    ld  c, a
    ld  hl, #_slotEmpty
    add a, l
    ld  l, a
    adc a, h
    sub a, l
    ld  h, a
    ld  a, (_dc_tw)
    ld  b, a
09202$:
    ld  a, (hl)
    inc hl
    or  a, a
    jr  nz, 09203$
    ld  a, (_dc_room)
    or  a, a
    jr  z, 09203$
    dec a
    ld  (_dc_room), a
    ld  0 (iy), e
    inc iy
    ld  0 (ix), d
    ld  1 (ix), c
    inc ix
    inc ix
09203$:
    ld  a, d
    add a, #8
    ld  d, a
    inc c
    djnz 09202$
    ld  a, c
    ld  (_dc_slot), a
    jr  09211$
09210$:
    ld  a, (_dc_tw)
    ld  c, a
    ld  a, (_dc_slot)
    add a, c
    ld  (_dc_slot), a
09211$:
    pop hl
    ld  de, #8
    add hl, de
    pop bc
    djnz 09201$
    jp  09290$
    ; ---------------- slow path: per-tile 16-bit clipping
09250$:
    ld  a, (_dc_th)
    ld  b, a
    ld  hl, (_dc_y0)
09251$:
    push bc
    push hl
    ld  de, #-17
    add hl, de
    ld  a, h
    or  a, a
    jr  nz, 09260$
    ld  a, l
    cp  a, #151
    jr  nc, 09260$
    add a, #16
    ld  (_dc_ybyte), a
    ld  hl, (_dc_x0)
    ld  a, (_dc_tw)
    ld  b, a
09252$:
    push hl
    ld  de, #-41
    add hl, de
    ld  a, h
    or  a, a
    jr  nz, 09253$
    ld  a, l
    cp  a, #167
    jr  nc, 09253$
    add a, #41
    ld  d, a
    ld  a, (_dc_slot)
    ld  c, a
    ld  hl, #_slotEmpty
    add a, l
    ld  l, a
    adc a, h
    sub a, l
    ld  h, a
    ld  a, (hl)
    or  a, a
    jr  nz, 09253$
    ld  a, (_dc_room)
    or  a, a
    jr  z, 09253$
    dec a
    ld  (_dc_room), a
    ld  a, (_dc_ybyte)
    ld  0 (iy), a
    inc iy
    ld  0 (ix), d
    ld  1 (ix), c
    inc ix
    inc ix
09253$:
    pop hl
    ld  de, #8
    add hl, de
    ld  a, (_dc_slot)
    inc a
    ld  (_dc_slot), a
    djnz 09252$
    jr  09261$
09260$:
    ld  a, (_dc_tw)
    ld  c, a
    ld  a, (_dc_slot)
    add a, c
    ld  (_dc_slot), a
09261$:
    pop hl
    ld  de, #8
    add hl, de
    pop bc
    djnz 09251$
09290$:
    ld  a, (_dc_room)
    ld  b, a
    ld  a, #64
    sub a, b
    ld  (_SpriteNextFree), a
09299$:
    pop iy
    pop ix
    ret
    __endasm;
}

#define draw_cell(sx, sy, slot, dims) \
    do { dc_slot = (slot); dc_dims = (dims); draw_cell_asm((sx), (sy)); } while (0)

static void add_one(s16 x, s16 y, u8 tile)
{
    dc_slot = tile;
    dc_dims = 0x11;
    draw_cell_asm(x, y);
}

/* Animation frame 0..frames-1, advancing every 8 game frames; `phase`
 * staggers when different objects change frame so their uploads spread out. */
static u8 anim_frame(u8 frames, u8 phase)
{
    u8 t;
    if (frames <= 1) return 0;
    t = (u8)((u16)(game.frame + phase) >> 3);
    while (t >= frames) t -= frames;   /* frames is small (<= 8) */
    return t;
}

/* World -> SMS screen.  Halving the position first (rather than the
 * difference) keeps sprites locked to the background, which scrolls by
 * camX >> 1. */
s16 baseX, baseY;
#define WX(x) ((s16)(baseX + ((s16)(x) >> 1)))
#define WY(y) ((s16)(baseY + ((s16)(y) >> 1)))

static u16 playerKey, boardKey;

static void draw_player(void)
{
    GGSprite art;
    u8 frame, n;
    u16 src;
    read_sprite(MISC_PLAYER + game.animState, &art);
    if (!art.frames) read_sprite(MISC_PLAYER, &art);
    n = (u8)((art.dims & 15) * (art.dims >> 4));
    frame = anim_frame(art.frames, 0);
    src = (game.player.facing ? art.tile : art.tileLeft) + (u16)frame * n;
    if (playerKey != src) {
        upload_sprite_tiles(src, SPR_PLAYER, n);
        playerKey = src;
    }
    if (game.invulnerability && (game.frame & 4)) return;
    draw_cell(WX(game.player.x + (PLAYER_W >> 1)) - (art.cellW >> 1),
              WY(game.player.y + PLAYER_H) - art.cellH, SPR_PLAYER, art.dims);
}

static void draw_board(void)
{
    GGSprite art;
    u8 frame, n;
    u16 src;
    if (!game.player.flying) return;
    read_sprite(MISC_SPECIALS + (game.player.facing ? SPECIAL_BOARD : SPECIAL_BOARD_LEFT), &art);
    if (!art.frames) return;
    n = (u8)((art.dims & 15) * (art.dims >> 4));
    if (n > 4) return;
    frame = anim_frame(art.frames, 0);
    src = art.tile + (u16)frame * n;
    if (boardKey != src) { upload_sprite_tiles(src, SPR_BOARD, n); boardKey = src; }
    draw_cell(WX(game.player.x + (PLAYER_W >> 1)) - (art.cellW >> 1), WY(game.player.y + 10),
              SPR_BOARD, art.dims);
}

/* ---- frame caches: blocks of sprite VRAM keyed by a frame's first pool
 * tile, recycled least-recently-used.  Returns the block's first sprite tile,
 * or 0xFF when the frame is not resident and the upload budget is spent. */
u16 enemyKey[ENEMY_BLOCKS];
u8 enemyAge[ENEMY_BLOCKS];
u16 itemKey[ITEM_BLOCKS];
static u8 itemAge[ITEM_BLOCKS];
u8 renderTick;

/* frame_cache stores the block it last returned here (0xFF if none). */
static u8 fcBlock;

static u8 frame_cache(u16 *keys, u8 *ages, u8 first, u8 count, u8 base, u8 blockTiles,
                      u16 src, u8 n)
{
    u8 b, best = 0xFF, bestAge = 0;
    for (b = first; b < count; b++) {
        if (keys[b] == src) {
            ages[b] = renderTick;
            fcBlock = b;
            return (u8)(base + b * blockTiles);
        }
    }
    fcBlock = 0xFF;
    if (uploadBudget < n) return 0xFF;
    for (b = first; b < count; b++) {
        u8 age;
        if (keys[b] == 0xFFFF) { best = b; break; }
        age = (u8)(renderTick - ages[b]);
        if (!age) continue;                       /* drawn this frame */
        if ((best == 0xFF) || (age > bestAge)) { best = b; bestAge = age; }
    }
    if (best == 0xFF) return 0xFF;
    uploadBudget -= n;
    keys[best] = src;
    ages[best] = renderTick;
    fcBlock = best;
    upload_sprite_tiles(src, (u8)(base + best * blockTiles), n);
    return (u8)(base + best * blockTiles);
}

/* Item art is shared by every pickup of one kind, so the descriptor, frame
 * and cache slot are worked out once per kind per frame. */
#define ITEM_TYPES 8
static u8 typeId[ITEM_TYPES], typeSlot[ITEM_TYPES], typeDims[ITEM_TYPES];
static s8 typeOffX[ITEM_TYPES], typeOffY[ITEM_TYPES];
static u8 typeCount;

static u8 item_type(u8 id, u8 klass)
{
    u8 t;
    GGSprite art;
    u8 n, frame;
    u16 src;
    for (t = 0; t < typeCount; t++)
        if (typeId[t] == id) return t;
    if (typeCount >= ITEM_TYPES) return 0xFF;
    t = typeCount++;
    typeId[t] = id;
    typeSlot[t] = 0xFF;
    read_sprite(MISC_EVSPRITES + id, &art);
    n = (u8)((art.dims & 15) * (art.dims >> 4));
    if (!art.frames || n > ITEM_BLOCK_TILES) return t;
    frame = (klass == JJ1_CLASS_BRIDGE) ? 0 : anim_frame(art.frames, (u8)(id << 1));
    src = art.tile;
    while (frame--) src += n;
    typeSlot[t] = frame_cache(itemKey, itemAge, 0, ITEM_CACHE, SPR_ITEM, ITEM_BLOCK_TILES, src, n);
    typeDims[t] = art.dims;
    typeOffX[t] = (s8)((16 - (s16)art.cellW) >> 1);
    typeOffY[t] = (s8)((16 - (s16)art.cellH) >> 1);
    return t;
}

static void draw_items(void)
{
    u8 i;
    ItemEntry *it = items;
    typeCount = 0;
    for (i = 0; i < listCount; ) {
        u8 k = it->klass, t;
        s16 sx, sy;
        if ((k == JJ1_CLASS_ITEM) && jazz_event_taken(it->gx, it->gy)) {
            /* collected for good: drop it from the list (last entry moves here) */
            listCount--;
            if (i != listCount) memcpy(it, &items[listCount], sizeof(ItemEntry));
            continue;
        }
        sx = baseX + it->px;
        sy = baseY + it->py;
        i++;
        it++;
        if (k == JJ1_CLASS_SPRING) {
            draw_cell(sx, sy + 8, it[-1].aux, 0x12);
            continue;
        }
        t = item_type(it[-1].id, k);
        if ((t == 0xFF) || (typeSlot[t] == 0xFF)) {
            if (k == JJ1_CLASS_ITEM) add_one(sx + 6, sy + 6, SPR_GEM);
            continue;
        }
        if (k == JJ1_CLASS_BRIDGE) {
            const Jj1EventInfo *info = &gg_einfo[it[-1].id];
            s16 deckY = sy + (info->points >> 1);
            u8 piece;
            s16 lx = sx;
            for (piece = 0; piece < info->param; piece++) {
                draw_cell(lx, deckY, typeSlot[t], typeDims[t]);
                lx += info->strength >> 1;
            }
        } else {
            draw_cell(sx + typeOffX[t], sy + typeOffY[t], typeSlot[t], typeDims[t]);
        }
    }
}

static void draw_companions(void)
{
    GGSprite art;
    u8 n, frame;
    u16 src;
    if (game.player.bird) {
        read_sprite(MISC_SPECIALS + (game.player.facing ? SPECIAL_BIRD : SPECIAL_BIRD_LEFT), &art);
        n = (u8)((art.dims & 15) * (art.dims >> 4));
        if (art.frames && n <= ITEM_BLOCK_TILES) {
            u8 slot = SPR_ITEM + (ITEM_BLOCKS - 1) * ITEM_BLOCK_TILES;
            frame = anim_frame(art.frames, 0);
            src = art.tile + (u16)frame * n;
            if (itemKey[ITEM_BLOCKS - 1] != src) {
                upload_sprite_tiles(src, slot, n);
                itemKey[ITEM_BLOCKS - 1] = src;
            }
            draw_cell(WX(game.player.birdX), WY(game.player.birdY), slot, art.dims);
        }
    }
    if (game.player.shield) {
        static const s8 ring[8][2] = {
            { 20, 0 }, { 14, 14 }, { 0, 20 }, { -14, 14 },
            { -20, 0 }, { -14, -14 }, { 0, -20 }, { 14, -14 }
        };
        read_sprite(MISC_SPECIALS + SPECIAL_SHIELD, &art);
        n = (u8)((art.dims & 15) * (art.dims >> 4));
        if (art.frames && n <= ITEM_BLOCK_TILES) {
            u8 phase = (u8)((game.frame >> 1) & 7);
            u8 orbs = (game.player.shield > 4) ? 4 : game.player.shield;
            u8 k, slot = SPR_ITEM + (ITEM_BLOCKS - 2) * ITEM_BLOCK_TILES;
            if (itemKey[ITEM_BLOCKS - 2] != art.tile) {
                upload_sprite_tiles(art.tile, slot, n);
                itemKey[ITEM_BLOCKS - 2] = art.tile;
            }
            for (k = 0; k < orbs; k++) {
                const s8 *o = ring[(phase + (k << 1)) & 7];
                draw_cell(WX(game.player.x + (PLAYER_W >> 1) - 4 + o[0]),
                          WY(game.player.y + (PLAYER_H >> 1) - 4 + o[1]), slot, art.dims);
            }
        }
    }
    if (game.player.invincibleTime) {
        static const s8 orbit[8][2] = {
            { 12, 0 }, { 8, 8 }, { 0, 12 }, { -8, 8 },
            { -12, 0 }, { -8, -8 }, { 0, -12 }, { 8, -8 }
        };
        u8 phase = (u8)((game.frame >> 1) & 7);
        u8 k;
        for (k = 0; k < 4; k++) {
            const s8 *o = orbit[(phase + (k << 1)) & 7];
            add_one(WX(game.player.x + (PLAYER_W >> 1) - 8 + o[0]),
                    WY(game.player.y + (PLAYER_H >> 1) - 8 + o[1]), SPR_GEM);
        }
    }
}

/* Per-enemy art cache, refreshed when the slot changes occupant. */

static u16 bossGunKey;

static void draw_boss_parts(const JazzEnemy *e, u8 id, u8 facingLeft, u8 charging,
                            s16 cellX, s16 cellY, u8 bodyTiles)
{
    GGSprite gun;
    u8 gidx = charging ? (facingLeft ? SPECIAL_BOSS_SPIKE_LEFT : SPECIAL_BOSS_SPIKE)
                       : (facingLeft ? SPECIAL_BOSS_CANNON_LEFT : SPECIAL_BOSS_CANNON);
    u8 oidx = (u8)((charging ? 4 : 0) + (facingLeft ? 2 : 0));
    s16 gx, gy;
    read_sprite(MISC_SPECIALS + gidx, &gun);
    SMS_mapROMBank(gg_miscBank);
    gx = MISC_BOSSOFFS[oidx];
    gy = MISC_BOSSOFFS[oidx + 1];
    if (gun.frames) {
        u8 gn = (u8)((gun.dims & 15) * (gun.dims >> 4));
        u8 gslot = (u8)(SPR_ENEMY + bodyTiles);
        u16 gsrc = gun.tile + (u16)anim_frame(gun.frames, 0) * gn;
        if (gn + bodyTiles <= ENEMY_BLOCK_TILES * JJ1_BOSS_SLOT_SPAN) {
            if (bossGunKey != gsrc) { upload_sprite_tiles(gsrc, gslot, gn); bossGunKey = gsrc; }
            draw_cell(cellX + (gx >> 1), cellY + (gy >> 1), gslot, gun.dims);
        }
    }
    {
        const Jj1EventInfo *bi = &gg_einfo[id];
        u8 total = bi->strength ? bi->strength : 1;
        u8 spent = (u8)((total > e->hitPoints) ? (total - e->hitPoints) : 0);
        s16 lost = (s16)(((s16)spent * 50) / total);
        s16 y = (s16)(GG_Y0 + 20 + lost);
        while (y < GG_Y0 + 70) {
            add_one(GG_X0 + 150, y, SPR_BOSSBAR);
            y += 8;
        }
    }
}

static u16 bossBodyKey;

/* Per-enemy render state, processed in a static working copy (SDCC reaches
 * a static struct with direct addressing, an array element only through
 * computed pointers). */
typedef struct {
    u8 gx, gy, id, artKey, block, last;
    u16 lastSrc;
    GGSprite art;
} EnemyRender;
EnemyRender er[JAZZ_MAX_ENEMIES];
static EnemyRender cr;
static JazzEnemy re;

static void draw_one_enemy(u8 i, u8 firstBlock)
{
    u8 facingLeft, n, frame, slot, charging = 0;
    u16 src;
    s16 cellX, cellY;
    if ((cr.gx != re.gridX) || (cr.gy != re.gridY)) {
        cr.gx = re.gridX;
        cr.gy = re.gridY;
        cr.id = gg_event(re.gridX, re.gridY);
        cr.last = 0xFF;
        cr.lastSrc = 0xFFFF;
        cr.artKey = 0xFF;
        cr.block = 0xFF;
    }
    facingLeft = (re.klass == JJ1_CLASS_BOSS) ? re.bossFacing : (re.direction < 0);
    if (re.klass == JJ1_CLASS_BOSS) {
        const Jj1EventInfo *bi = &gg_einfo[cr.id];
        u8 total = bi->strength ? bi->strength : 2;
        charging = (u8)((total - re.hitPoints) >= (total >> 1));
    }
    {
        u8 key = (u8)(charging | (facingLeft << 1));
        if (cr.artKey != key) {
            read_sprite(MISC_EVSPRITES + cr.id, &cr.art);
            if (charging) {
                GGSprite late;
                read_sprite(MISC_SPECIALS + (facingLeft ? SPECIAL_BOSS_LATE_LEFT : SPECIAL_BOSS_LATE), &late);
                if (late.frames) cr.art = late;
            }
            cr.artKey = key;
        }
    }
    if (!cr.art.frames) {
        add_one(WX(re.x), WY(re.y), SPR_GEM);
        return;
    }
    n = (u8)((cr.art.dims & 15) * (cr.art.dims >> 4));
    frame = anim_frame(cr.art.frames, (u8)(i * 3));
    src = facingLeft ? cr.art.tileLeft : cr.art.tile;
    while (frame--) src += n;
    if (re.klass == JJ1_CLASS_BOSS) {
        /* The guardian owns the first three blocks (body + accessory). */
        if (n > ENEMY_BLOCK_TILES * JJ1_BOSS_SLOT_SPAN) return;
        if (bossBodyKey != src) { upload_sprite_tiles(src, SPR_ENEMY, n); bossBodyKey = src; }
        slot = SPR_ENEMY;
    } else {
        u8 blk = cr.block;
        if (n > ENEMY_BLOCK_TILES) return;
        if ((src == cr.lastSrc) && (blk != 0xFF) && (enemyKey[blk] == src)) {
            slot = cr.last;
            enemyAge[blk] = renderTick;
        } else {
            slot = frame_cache(enemyKey, enemyAge, firstBlock, ENEMY_BLOCKS, SPR_ENEMY,
                               ENEMY_BLOCK_TILES, src, n);
            if (slot == 0xFF) slot = cr.last;   /* keep the old frame */
            else { cr.lastSrc = src; cr.block = fcBlock; }
            if (slot == 0xFF) return;
        }
    }
    cr.last = slot;
    cellX = WX(re.x + (JAZZ_ENEMY_W >> 1)) - (cr.art.cellW >> 1);
    cellY = WY(re.y + JAZZ_ENEMY_H) - cr.art.cellH;
    draw_cell(cellX, cellY, slot, cr.art.dims);
    if (re.klass == JJ1_CLASS_BOSS) draw_boss_parts(&re, cr.id, facingLeft, charging, cellX, cellY, n);
}

/* Steady-state enemy drawing in Z80 (the C draw_one_enemy handles new
 * occupants, art or facing changes, frame-cache misses and the guardian):
 * same anim frame, source tile, cache check and placement as the C.
 * A = slot index; returns 0 when drawn, 1 to fall back to C. */
#include <stddef.h>
#define OFF_FRAME 2470
typedef char chk_frame[(offsetof(JazzGame, frame) == OFF_FRAME) ? 1 : -1];
typedef char chk_er[(sizeof(EnemyRender) == 16) ? 1 : -1];
typedef char chk_en[(offsetof(JazzGame, enemies) == 39) ? 1 : -1];
u8 ef_i, ef_t, ef_n;

static u8 enemy_fast(u8 i) __naked
{
    (void)i;
    __asm
    push ix
    push iy
    ld  (_ef_i), a
    ld  l, a
    ld  h, #0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    push hl
    ld  de, #(_game + 39)
    add hl, de
    push hl
    pop ix
    pop hl
    ld  de, #_er
    add hl, de
    push hl
    pop iy
    ld  a, 0 (iy)
    cp  a, 11 (ix)
    jp  nz, 09790$
    ld  a, 1 (iy)
    cp  a, 12 (ix)
    jp  nz, 09790$
    ld  a, 8 (ix)
    rlca
    and a, #1
    ld  c, a
    add a, a
    cp  a, 3 (iy)
    jp  nz, 09790$
    ld  a, 12 (iy)
    or  a, a
    jp  z, 09790$
    ld  b, a
    ld  a, (_ef_i)
    ld  e, a
    add a, a
    add a, e
    ld  e, a
    ld  d, #0
    ld  hl, (_game + OFF_FRAME)
    add hl, de
    srl h
    rr  l
    srl h
    rr  l
    srl h
    rr  l
    ld  a, l
09701$:
    cp  a, b
    jr  c, 09702$
    sub a, b
    jr  09701$
09702$:
    ld  (_ef_t), a
    ld  a, 13 (iy)
    and a, #15
    ld  e, a
    ld  a, 13 (iy)
    rrca
    rrca
    rrca
    rrca
    and a, #15
    jp  z, 09790$
    ld  b, a
    xor a, a
09703$:
    add a, e
    djnz 09703$
    ld  (_ef_n), a
    ld  a, c
    or  a, a
    jr  nz, 09704$
    ld  l, 8 (iy)
    ld  h, 9 (iy)
    jr  09705$
09704$:
    ld  l, 10 (iy)
    ld  h, 11 (iy)
09705$:
    ld  a, (_ef_t)
    or  a, a
    jr  z, 09707$
    ld  b, a
    ld  a, (_ef_n)
    ld  e, a
    ld  d, #0
09706$:
    add hl, de
    djnz 09706$
09707$:
    ld  a, 6 (iy)
    cp  a, l
    jr  nz, 09790$
    ld  a, 7 (iy)
    cp  a, h
    jr  nz, 09790$
    ld  a, 4 (iy)
    cp  a, #0xFF
    jr  z, 09790$
    ld  e, a
    ld  d, #0
    push hl
    ld  hl, #_enemyKey
    add hl, de
    add hl, de
    ld  a, (hl)
    inc hl
    ld  b, (hl)
    pop hl
    cp  a, l
    jr  nz, 09790$
    ld  a, b
    cp  a, h
    jr  nz, 09790$
    ld  hl, #_enemyAge
    add hl, de
    ld  a, (_renderTick)
    ld  (hl), a
    ld  a, 5 (iy)
    ld  (_dc_slot), a
    ld  a, 13 (iy)
    ld  (_dc_dims), a
    ld  l, 0 (ix)
    ld  h, 1 (ix)
    ld  de, #7
    add hl, de
    sra h
    rr  l
    ld  de, (_baseX)
    add hl, de
    ld  a, 14 (iy)
    srl a
    ld  e, a
    ld  d, #0
    or  a, a
    sbc hl, de
    push hl
    ld  l, 2 (ix)
    ld  h, 3 (ix)
    ld  de, #16
    add hl, de
    sra h
    rr  l
    ld  de, (_baseY)
    add hl, de
    ld  e, 15 (iy)
    ld  d, #0
    or  a, a
    sbc hl, de
    ex  de, hl
    pop hl
    call _draw_cell_asm
    xor a, a
    jr  09791$
09790$:
    ld  a, #1
09791$:
    pop iy
    pop ix
    ret
    __endasm;
}

static void draw_enemies(void)
{
    u8 k, i;
    /* rotate the draw order so the 8-sprites-per-line limit flickers fairly */
    u8 start = (u8)(game.frame & 7);
    u8 firstBlock = game.bossPresent ? JJ1_BOSS_SLOT_SPAN : 0;
    while (start >= JAZZ_MAX_ENEMIES) start -= JAZZ_MAX_ENEMIES;
    i = start;
    for (k = 0; k < JAZZ_MAX_ENEMIES; k++) {
        const JazzEnemy *e = &game.enemies[i];
        if (e->active && ((e->klass == JJ1_CLASS_BOSS) || enemy_fast(i))) {
            EnemyRender *r = &er[i];
            memcpy(&re, e, sizeof(JazzEnemy));
            memcpy(&cr, r, sizeof(EnemyRender));
            draw_one_enemy(i, firstBlock);
            memcpy(r, &cr, sizeof(EnemyRender));
        }
        i++;
        if (i == JAZZ_MAX_ENEMIES) i = 0;
    }
}

static void draw_bullets(void)
{
    u8 i;
    const JazzBullet *b = game.bullets;
    for (i = 0; i < JAZZ_MAX_BULLETS; i++, b++)
        if (b->active) add_one(WX(b->x) - 2, WY(b->y) - 2, SPR_BULLET);
}

/* ------------------------------------------------------------------ HUD */
static u8 glyph(char c)
{
    u8 i;
    for (i = 0; HUD_GLYPHS[i]; i++)
        if (HUD_GLYPHS[i] == c) return i;
    return 0;
}

static void hud_text(u8 x, u8 y, const char *s)
{
    while (*s) {
        if (*s != ' ') SMS_addSprite(x, y, SPR_FONT + glyph(*s));
        x += 8;
        s++;
    }
}

static u16 hudScore = 0xFFFF;
static u8 scoreGlyph[5];

static void draw_hud(void)
{
    static const u8 weaponGlyph[JAZZ_SHOT_TYPES] = { 12, 13, 14, 15, 16 };  /* B T M O S */
    u8 i, x;
    SMS_addSprite(GG_X0 + 2, GG_Y0 + 2, SPR_FONT + G_HEART);
    SMS_addSprite(GG_X0 + 10, GG_Y0 + 2, SPR_FONT + game.health);
    SMS_addSprite(GG_X0 + 22, GG_Y0 + 2, SPR_FONT + G_FACE);           /* Jazz */
    SMS_addSprite(GG_X0 + 30, GG_Y0 + 2, SPR_FONT + (game.lives > 9 ? 9 : game.lives));
    if (game.player.shotType)
        SMS_addSprite(GG_X0 + 150, GG_Y0 + 2, SPR_FONT + weaponGlyph[game.player.shotType]);
    if (hudScore != game.score) {
        u16 v = game.score;
        hudScore = v;
        i = 5;
        while (i) { i--; scoreGlyph[i] = (u8)(SPR_FONT + (v % 10)); v /= 10; }
    }
    for (i = 0, x = GG_X0 + 2; i < 5; i++, x += 8) SMS_addSprite(x, GG_Y0 + 134, scoreGlyph[i]);
    if (game.paused) hud_text(GG_X0 + 56, GG_Y0 + 68, "PAUSED");
    else if (game.transitionTimer) hud_text(GG_X0 + 44, GG_Y0 + 68, "WELL DONE");
}

/* ------------------------------------------------------------- loading */
/* Draw font glyphs into VRAM as 4bpp tiles in colours fg / shadow.  With
 * `subset`, only the HUD's glyphs (HUD_GLYPHS order), else the whole font. */
static u8 font_index(char c)
{
    u8 i;
    for (i = 0; FONT_CHARS[i]; i++)
        if (FONT_CHARS[i] == c) return i;
    return 0;
}

static void load_font(u16 vramTile, u8 fg, u8 shadow, u8 subset)
{
    u8 buf[32];
    u8 c, y, k;
    u8 count = subset ? (u8)(sizeof(HUD_GLYPHS) - 1) : FONT_COUNT;
    for (c = 0; c < count; c++) {
        const u8 *src = gg_font + ((u16)(subset ? font_index(HUD_GLYPHS[c]) : c) << 3);
        for (y = 0; y < 8; y++) {
            u8 v = src[y];
            u8 sh = (u8)((y ? (src[y - 1] >> 1) : 0) & ~v);
            for (k = 0; k < 4; k++) {
                u8 p = 0;
                if (fg & (1 << k)) p |= v;
                if (shadow & (1 << k)) p |= sh;
                buf[y * 4 + k] = p;
            }
        }
        SMS_loadTiles(buf, vramTile + c, 32);
    }
}

void render_load_font_bg(void)
{
    load_font(0, 1, 2, 0);
}

static void hide_sprites(void)
{
    SMS_initSprites();
    SMS_finalizeSprites();
    SMS_copySpritestoSAT();
}

/* Called with the display off whenever the stage (and so the art) changes. */
void render_stage_begin(void)
{
    const GGStageInfo *si = &gg_stage_info[game.stage];
    const GGWorldInfo *wi = &gg_world_info[si->world];
    u8 i;
    hide_sprites();
    GG_loadBGPalette(wi->bgPal);
    GG_loadSpritePalette(si->spritePal);
    curTileBank = wi->tileBank;
    hudBright = si->hudBright;
    hudDark = si->hudDark;
    load_font(256 + SPR_FONT, hudBright, hudDark, 1);
    {
        GGSprite art;
        for (i = 0; i < 3; i++) {
            read_sprite(MISC_SPECIALS + SPECIAL_SPRING0 + i, &art);
            upload_sprite_tiles(art.tile, SPR_SPRING + i * 2, 2);
        }
        read_sprite(MISC_SPECIALS + SPECIAL_GEM, &art);
        upload_sprite_tiles(art.tile, SPR_GEM, 1);
        read_sprite(MISC_SPECIALS + SPECIAL_BULLET, &art);
        upload_sprite_tiles(art.tile, SPR_BULLET, 1);
        read_sprite(MISC_SPECIALS + SPECIAL_BOSSBAR, &art);
        upload_sprite_tiles(art.tile, SPR_BOSSBAR, 1);
    }
    for (i = 0; i < ENEMY_BLOCKS; i++) enemyKey[i] = 0xFFFF;
    for (i = 0; i < ITEM_BLOCKS; i++) itemKey[i] = 0xFFFF;
    for (i = 0; i < JAZZ_MAX_ENEMIES; i++) {
        er[i].gx = 0xFF;
        er[i].last = 0xFF;
        er[i].lastSrc = 0xFFFF;
        er[i].artKey = 0xFF;
        er[i].block = 0xFF;
    }
    playerKey = boardKey = bossGunKey = bossBodyKey = 0xFFFF;
    renderedDestroyed = 0;
    hudScore = 0xFFFF;
    /* empty tile 0 (sky) and a clean nametable */
    SMS_VRAMmemset(0, 0, 32);
    SMS_VRAMmemsetW(0x3800, 0, 32 * 28 * 2);
    cache_reset();
    winX = winY = -100;
    render_camera_snap();
}

static s16 camera_follow(s16 camera, s16 wanted)
{
    s16 delta = wanted - camera;
    s16 step;
    if (!delta) return camera;
    step = delta >> 2;
    if (!step) step = (delta > 0) ? 1 : -1;
    return (s16)(camera + step);
}

/* The GG screen shows 320x288 original pixels. */
#define VIEW_W 320
#define VIEW_H 288

static s16 wanted_x(void)
{
    s16 w = game.player.x - 153;
    if (w < 0) w = 0;
    if (w > (256 * 32) - VIEW_W) w = (256 * 32) - VIEW_W;
    return w;
}
static s16 wanted_y(void)
{
    s16 w = game.player.y - 124;
    if (w < 0) w = 0;
    if (w > (64 * 32) - VIEW_H) w = (64 * 32) - VIEW_H;
    return w;
}

static void update_scroll_values(void)
{
    s16 sx = camX >> 1, sy = camY >> 1;
    u16 m = (u16)(sy - GG_Y0 + 224 * 5);
    while (m >= 224) m -= 224;
    scrollX = (u8)(GG_X0 - sx);
    scrollY = (u8)m;
}

void render_camera_snap(void)
{
    camX = wanted_x();
    camY = wanted_y();
    window_move(camX >> 5, camY >> 5);
    update_scroll_values();
    SMS_setBGScrollX(scrollX);
    SMS_setBGScrollY(scrollY);
}

/* Right after VBlank: latch scroll and the sprite table built last frame. */
void render_vblank(void)
{
    SMS_setBGScrollX(scrollX);
    SMS_setBGScrollY(scrollY);
    UNSAFE_SMS_copySpritestoSAT();
}

/* Everything for the next frame (map streaming, sprite list).  `steps` is
 * how many game frames the camera should catch up by. */
void render_frame(u8 steps)
{
    cacheMisses = 0;
    spriteUploads = 0;
    winMoved = 0;
    renderTick++;
    while (steps--) {
        camX = camera_follow(camX, wanted_x());
        camY = camera_follow(camY, wanted_y());
    }
    apply_destroyed();
    window_move(camX >> 5, camY >> 5);
    update_scroll_values();
    baseX = GG_X0 - (camX >> 1);
    baseY = GG_Y0 - (camY >> 1);

    uploadBudget = 16;
    SMS_initSprites();
    draw_hud();
    draw_player();
    draw_board();
    draw_bullets();
    draw_companions();
    draw_enemies();
    draw_items();
    SMS_finalizeSprites();
}

void render_init(void)
{
    SMS_useFirstHalfTilesforSprites(0);
    SMS_setSpriteMode(SPRITEMODE_NORMAL);
}
