/* Fixed-ROM half of the bonus levels: the map, collision and event lookups
 * and the perspective floor sampler, which page the level's data banks into
 * slot 2 (the bonus code itself runs from bank 4 in that same window).
 *
 * Map bank layout (tools/bonus.py): 1024 chunk numbers (32x32 chunks of
 * 8x8 tiles), the 64-byte chunks, 60 x 8 collision rows (a bit per 4x4 px
 * cell), 257 event row starts, then (column, id) pairs row by row. */

#include <SMSlib.h>
#include "jazz_game.h"
#include "gg_data.h"
#include "bonus_fixed.h"

u8 bxBank;
u16 bxRefs, bxChunks, bxMasks, bxRows;

u8 grid_tile(u8 tx, u8 ty)
{
    const u8 *refs = (const u8 *)bxRefs;
    u8 c = refs[((u16)(ty >> 3) << 5) | (tx >> 3)];
    return ((const u8 *)bxChunks)[((u16)c << 6) | ((ty & 7) << 3) | (tx & 7)];
}

u8 bx_tile(u8 tx, u8 ty)
{
    u8 save = SMS_getROMBank(), t;
    SMS_mapROMBank(bxBank);
    t = grid_tile(tx, ty);
    SMS_mapROMBank(save);
    return t;
}

/* Collision row `row` (0..7) of floor tile `tile`: bit c = cell column c. */
u8 bx_mask(u8 tile, u8 row)
{
    u8 save = SMS_getROMBank(), m;
    SMS_mapROMBank(bxBank);
    m = ((const u8 *)bxMasks)[((u16)tile << 3) | row];
    SMS_mapROMBank(save);
    return m;
}

/* Collision probe of the bonus player: the tile's mask cell (row, col) and
 * the event at the map cell (bxProbeEvent), in one bank switch. */
u8 bxProbeEvent;
u8 bx_probe(u8 tx, u8 ty, u8 row, u8 col)
{
    u8 save = SMS_getROMBank(), t, m, id = 0;
    const u8 *p, *end;
    SMS_mapROMBank(bxBank);
    t = grid_tile(tx, ty);
    if (t > 59) t = 59;
    m = (u8)(((const u8 *)bxMasks)[((u16)t << 3) | row] & (1 << col));
    p = (const u8 *)((const u16 *)bxRows)[ty];
    end = (const u8 *)((const u16 *)bxRows)[ty + 1];
    for (; p != end; p += 2) {
        if (p[0] == tx) { id = p[1]; break; }
        if (p[0] > tx) break;
    }
    bxProbeEvent = id;
    SMS_mapROMBank(save);
    return m;
}

/* Event id at a map cell (0 = none), ignoring collection. */
u8 bx_event(u8 tx, u8 ty)
{
    u8 save = SMS_getROMBank(), id = 0;
    const u8 *p, *end;
    SMS_mapROMBank(bxBank);
    p = (const u8 *)((const u16 *)bxRows)[ty];
    end = (const u8 *)((const u16 *)bxRows)[ty + 1];
    for (; p != end; p += 2) {
        if (p[0] == tx) { id = p[1]; break; }
        if (p[0] > tx) break;
    }
    SMS_mapROMBank(save);
    return id;
}

/* Events of map row ty with column in the (wrapping) window [x0, x0+n):
 * writes (offset from x0, id) pairs to out, returns how many. */
u8 bx_row_events(u8 ty, u8 x0, u8 n, u8 *out, u8 max)
{
    u8 save = SMS_getROMBank(), k = 0;
    const u8 *p, *end;
    SMS_mapROMBank(bxBank);
    p = (const u8 *)((const u16 *)bxRows)[ty];
    end = (const u8 *)((const u16 *)bxRows)[ty + 1];
    for (; p != end && k < max; p += 2) {
        u8 off = (u8)(p[0] - x0);
        if (off < n) { out[0] = off; out[1] = p[1]; out += 2; k++; }
    }
    SMS_mapROMBank(save);
    return k;
}

/* ---------------------------------------------------------------- floor */
/* One sample row: 40 samples of the floor from (u, v) in steps (du, dv).
 * u, v are 1/2048 px in 24 bits, wrapping with the 8192 px map, so the map
 * tile is the top byte and the 16x16 texel the top nibble of the middle
 * byte.  Two samples make one nametable entry: the pair tile
 * (left << 4 | right). */
u32 fsU, fsV, fsDU, fsDV;
u8 *fsOut;
u8 fsLastTx, fsLastTy, fsLastTile;

u8 fsCnt, fsT;

/* Hand-written: the 400 samples a frame are most of the bonus level's time.
 * The u coordinate lives in the main registers (H = tile, L = texel << 4 |
 * fraction, D = low fraction; its step in B, C, E), v in the alternate set;
 * a sample's texel is at 0x8000 + tile * 256 + (v1 & 0xF0) + (u1 >> 4) in
 * the texture bank, the tile from a one-entry cache or the map bank. */
void bx_floor_row(void) __naked
{
    __asm
    push ix
    push iy
    ld  a, (0xFFFF)
    push af
    ld  iy, (_fsOut)
    ld  hl, (_fsV + 1)
    ld  bc, (_fsDV + 1)
    ld  a, (_fsV)
    ld  d, a
    ld  a, (_fsDV)
    ld  e, a
    exx
    ld  hl, (_fsU + 1)
    ld  bc, (_fsDU + 1)
    ld  a, (_fsU)
    ld  d, a
    ld  a, (_fsDU)
    ld  e, a
    ld  a, #BONUS_TEX_BANK
    ld  (0xFFFF), a
    ld  a, #20
    ld  (_fsCnt), a
09830$:
    call 09840$
    rlca
    rlca
    rlca
    rlca
    ld  0 (iy), a
    call 09840$
    or  a, 0 (iy)
    ld  0 (iy), a
    inc iy
    ld  a, (_fsCnt)
    dec a
    ld  (_fsCnt), a
    jr  nz, 09830$
    pop af
    ld  (0xFFFF), a
    pop iy
    pop ix
    ret
    ; ---- one sample: A = texel; steps u and v
09840$:
    ld  a, (_fsLastTx)
    cp  a, h
    jr  nz, 09845$
    exx
    ld  a, (_fsLastTy)
    cp  a, h
    jr  nz, 09844$
    ld  a, l
    and a, #0xF0
    exx
    ld  (_fsT), a
    jr  09848$
09844$:
    exx
09845$:
    ld  a, h
    ld  (_fsLastTx), a
    exx
    ld  a, h
    ld  (_fsLastTy), a
    ld  a, l
    and a, #0xF0
    exx
    ld  (_fsT), a
    push hl
    push de
    push bc
    push iy
    ld  a, (_bxBank)
    ld  (0xFFFF), a
    ld  a, (_fsLastTy)
    ld  l, a
    ld  a, (_fsLastTx)
    call _grid_tile
    ld  (_fsLastTile), a
    ld  a, #BONUS_TEX_BANK
    ld  (0xFFFF), a
    pop iy
    pop bc
    pop de
    pop hl
09848$:
    ld  a, l
    rrca
    rrca
    rrca
    rrca
    and a, #0x0F
    push hl
    ld  hl, #_fsT
    or  a, (hl)
    ld  l, a
    ld  a, (_fsLastTile)
    add a, #0x80
    ld  h, a
    ld  a, (hl)
    pop hl
    ex  af, af
    ld  a, d
    add a, e
    ld  d, a
    adc hl, bc
    exx
    ld  a, d
    add a, e
    ld  d, a
    adc hl, bc
    exx
    ex  af, af
    ret
    __endasm;
}

void bx_floor_reset(void)
{
    u8 save = SMS_getROMBank();
    fsLastTx = fsLastTy = 0;
    SMS_mapROMBank(bxBank);
    fsLastTile = grid_tile(0, 0);
    SMS_mapROMBank(save);
}

/* Floor buffer (10 rows of 20 pair tiles) to the nametable at GG row 8.
 * Called right after VBlank. */
__sfr __at 0xBF BxCtrl;
__sfr __at 0xBE BxData;
u8 *fuBuf;
static void floor_upload_asm(void) __naked
{
    __asm
    ld  hl, (_fuBuf)
    ld  de, #(0x3800 + (11 << 6) + 12)
    ld  b, #10
09860$:
    push bc
    di
    ld  a, e
    out (0xBF), a
    ld  a, d
    or  a, #0x40
    out (0xBF), a
    ld  b, #20
09861$:
    ld  a, (hl)
    out (0xBE), a
    inc hl
    xor a, a
    out (0xBE), a
    djnz 09861$
    ei
    ex  de, hl
    ld  bc, #64
    add hl, bc
    ex  de, hl
    pop bc
    djnz 09860$
    ret
    __endasm;
}

void bx_floor_upload(const u8 *buf)
{
    fuBuf = (u8 *)buf;
    floor_upload_asm();
}

/* ---------------------------------------------------------------- raster */
/* Line interrupt every 8 lines: the sky gradient (colour 0) down the sky
 * rows, the horizon strip scrolled with the player's heading, the HUD row
 * and the floor unscrolled.  Band k fires at the end of line 8k + 7. */
u8 bxBand, bxStripScroll;
u16 bxSky[6];
__sfr __at 0x7E BxVCount;


/* Decided by the beam's line (V counter), so a late interrupt (one that
 * waited for a VRAM copy with interrupts off) still does the right thing. */
void bx_line_irq(void)
{
    u8 line = BxVCount;
    if (line < 30) return;
    if (line < 84) {
        u8 k = (u8)((line - 30) >> 3);
        u16 c;
        if (k > 5) k = 5;
        c = bxSky[k];
        BxCtrl = 0;
        BxCtrl = 0xC0;
        BxData = (u8)c;
        BxData = (u8)(c >> 8);
        BxCtrl = bxStripScroll; BxCtrl = 0x88;
    } else {
        BxCtrl = 0; BxCtrl = 0x88;
        BxCtrl = 255; BxCtrl = 0x8A;    /* no more line interrupts this frame */
    }
}

void bx_frame_irq(void)
{
    bxBand = 0xFF;
    BxCtrl = 0; BxCtrl = 0x88;
    BxCtrl = 7; BxCtrl = 0x8A;
    BxCtrl = 0; BxCtrl = 0xC0;              /* colour 0: the top sky band */
    BxData = (u8)bxSky[0];
    BxData = (u8)(bxSky[0] >> 8);
}


/* VRAM copies safe against the raster interrupt (which rewrites the VDP
 * address): address and data with interrupts off.  `bank` 0 copies from RAM
 * or fixed ROM, else pages that ROM bank in. */
u16 vcSrc, vcDst, vcN;
static void vram_copy_asm(void) __naked
{
    __asm
    di
    ld  hl, (_vcDst)
    ld  a, l
    out (0xBF), a
    ld  a, h
    or  a, #0x40
    out (0xBF), a
    ld  hl, (_vcSrc)
    ld  de, (_vcN)
    ld  c, #0xBE
    ; whole 256-byte blocks, then the rest (28 cycles a byte)
    ld  b, e
    inc d
    ld  a, e
    or  a, a
    jr  nz, 09850$
    dec d
    jr  z, 09851$
09850$:
    outi
    jr  nz, 09850$
    dec d
    jr  nz, 09850$
09851$:
    ei
    ret
    __endasm;
}

void bx_vram_copy(u8 bank, u16 src, u16 dst, u16 n)
{
    u8 save = SMS_getROMBank();
    if (bank) SMS_mapROMBank(bank);
    vcSrc = src;
    vcDst = dst;
    vcN = n;
    vram_copy_asm();
    SMS_mapROMBank(save);
}

/* The sprite table (SMSlib's buffers) to the SAT. */
extern unsigned char SpriteTableY[64], SpriteTableXN[128];
extern unsigned char SpriteNextFree;
void bx_sat(void)
{
    u8 n = SpriteNextFree;
    if (n < 64) SpriteTableY[n] = 0xD0;
    bx_vram_copy(0, (u16)SpriteTableY, 0x3F00, (n < 64) ? n + 1 : 64);
    if (n) bx_vram_copy(0, (u16)SpriteTableXN, 0x3F80, (u16)n << 1);
}
