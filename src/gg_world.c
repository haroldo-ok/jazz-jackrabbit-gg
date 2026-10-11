/* Fixed-ROM accessors for the banked level data.
 *
 * Each level has three 16 KB banks of tables (see tools/convert.py):
 *   cell bank  - 256x64 bytes, the baked collision/render cell id per block
 *   event bank - 256x64 bytes, the original JJ1 event grid
 *   misc bank  - per-cell-id masks, block tile refs, event records, sprites
 * Every accessor saves and restores the slot-2 bank, because the game logic
 * that calls them itself runs from slot 2. */

#include <SMSlib.h>
#include "jazz_game.h"
#include "gg_data.h"

Jj1EventInfo gg_einfo[128];
u8 gg_cellBank, gg_eventBank, gg_miscBank, gg_sprBank, gg_worldNo;

/* One-entry cache of the last block looked up: probes cluster heavily in the
 * two or three blocks around the player. */
static u8 lastBx = 0xFF, lastBy = 0xFF, lastId;

static const u8 bitmask[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
/* Set while probing the raw mask (destroyed blocks still solid), as the
 * Genesis guardian's drill does through jj1_runtime_down_point_solid. */
u8 probeRaw;

void gg_world_load(u8 stage)
{
    const GGStageInfo *si = &gg_stage_info[stage];
    u8 save = SMS_getROMBank();
    u8 *dst = (u8 *)gg_einfo;
    const u8 *src;
    u16 i;
    gg_cellBank = si->cellBank;
    gg_eventBank = si->eventBank;
    gg_miscBank = si->miscBank;
    gg_sprBank = si->sprBank;
    gg_worldNo = si->world;
    SMS_mapROMBank(gg_miscBank);
    src = MISC_EVENTINFO;
    for (i = 0; i < 512; i++) *dst++ = *src++;
    SMS_mapROMBank(save);
    lastBx = 0xFF;
}

void gg_stage_facts(u8 stage, u8 *sx, u8 *sy, u16 *items, u8 *bossPresent, u8 *bx, u8 *by)
{
    const GGStageInfo *si = &gg_stage_info[stage];
    *sx = si->startX;
    *sy = si->startY;
    *items = si->items;
    *bossPresent = si->bossPresent;
    *bx = si->bossX;
    *by = si->bossY;
}

/* u8 gg_event(s16 bx, s16 by): event id of a block, 0 outside the map.
 * Hand-written: it is called for every grid query the logic makes.
 * sdcccall(1): HL = bx, DE = by, result in A. */
u8 gg_event(s16 bx, s16 by) __naked
{
    (void)bx; (void)by;
    __asm
    ld  a, h
    or  a, d
    jr  nz, 09109$
    ld  a, e
    cp  a, #64
    jr  nc, 09109$
    add a, #0x80
    ld  h, a
    ld  a, (0xFFFF)
    ld  c, a
    ld  a, (_gg_eventBank)
    ld  (0xFFFF), a
    ld  b, (hl)
    ld  a, c
    ld  (0xFFFF), a
    ld  a, b
    ret
09109$:
    xor a, a
    ret
    __endasm;
}

/* Cell id of a 32 px block (bank switching included). */
u8 gg_cell_id(u8 bx, u8 by)
{
    u8 save, id;
    save = SMS_getROMBank();
    SMS_mapROMBank(gg_cellBank);
    {
        u16 off = by;
        off <<= 8;
        off |= bx;
        id = *((const u8 *)(0x8000 + off));
    }
    SMS_mapROMBank(save);
    return id;
}

/* Item collected / enemy killed at this event cell.  Lives in fixed ROM:
 * the renderer calls it too, while a data bank is in the slot-2 window. */
u8 jazz_event_taken(u8 gridX, u8 gridY)
{
    u16 bit = gridY;
    bit <<= 8;
    bit |= gridX;
    return (u8)((game.taken[bit >> 3] & bitmask[gridX & 7]) ? 1 : 0);
}

u8 gg_cell_destroyed(u8 gx, u8 gy)
{
    u8 i, n;
    if (!(game.destroyedCols[gx >> 3] & bitmask[gx & 7])) return 0;
    n = game.destroyedCount;
    for (i = 0; i < n; i++)
        if ((game.destroyedX[i] == gx) && (game.destroyedY[i] == gy)) return 1;
    return 0;
}

u8 prCol;

/* Collision probe, hand-written because the game makes ~100 of these a
 * frame.  u8 gg_solid(s16 x, s16 y) / gg_down_solid: HL = x, DE = y.
 *   1 outside the map (x < 0, x >= 8192, y < 0, y >= 2048)
 *   else bit ((x>>2)&7) of row ((y>>2)&7) of the cell id's 8-byte mask,
 *   where the cell id comes from the cell bank at 0x8000 + by*256 + bx and
 *   the masks from the misc bank (solid at 0x8100, feet-only at 0x8900),
 *   and 0 if the block was destroyed.  The slot-2 bank is preserved. */
static void probe_common(void) __naked
{
    __asm
    ld  a, h
    and a, #0xE0
    jp  nz, 09190$
    ld  a, d
    and a, #0xF8
    jp  nz, 09190$
    ld  a, l
    rrca
    rrca
    and a, #0x07
    ld  (_prCol), a
    ld  a, l
    rlca
    rlca
    rlca
    and a, #0x07
    ld  b, a
    ld  a, h
    add a, a
    add a, a
    add a, a
    or  a, b
    ld  b, a
    ld  a, e
    rrca
    rrca
    and a, #0x07
    ld  l, a
    ld  a, e
    rlca
    rlca
    rlca
    and a, #0x07
    ld  e, a
    ld  a, d
    add a, a
    add a, a
    add a, a
    or  a, e
    ld  d, a
    ld  a, (0xFFFF)
    ld  e, a
    ld  a, (_lastBx)
    cp  a, b
    jr  nz, 09101$
    ld  a, (_lastBy)
    cp  a, d
    jr  z, 09102$
09101$:
    ld  a, b
    ld  (_lastBx), a
    ld  a, d
    ld  (_lastBy), a
    ld  a, (_gg_cellBank)
    ld  (0xFFFF), a
    push hl
    ld  a, d
    add a, #0x80
    ld  h, a
    ld  l, b
    ld  a, (hl)
    ld  (_lastId), a
    pop hl
09102$:
    ld  a, (_lastId)
    ld  h, a
    rlca
    rlca
    rlca
    and a, #0x07
    add a, c
    ld  c, a
    ld  a, h
    add a, a
    add a, a
    add a, a
    or  a, l
    ld  l, a
    ld  h, c
    ld  a, (_gg_miscBank)
    ld  (0xFFFF), a
    ld  c, (hl)
    ld  a, e
    ld  (0xFFFF), a
    ld  a, (_prCol)
    ld  hl, #_bitmask
    add a, l
    ld  l, a
    adc a, h
    sub a, l
    ld  h, a
    ld  a, (hl)
    and a, c
    ret z
    ld  a, (_probeRaw)
    or  a, a
    jr  nz, 09104$
    ld  a, (_game)
    or  a, a
    jr  nz, 09103$
09104$:
    inc a
    ret
09103$:
    ld  a, b
    ld  l, d
    call _gg_cell_destroyed
    xor a, #1
    ret
09190$:
    ld  a, #1
    ret
    __endasm;
}


u8 gg_down_solid_raw(s16 x, s16 y)
{
    u8 r;
    probeRaw = 1;
    r = gg_down_solid(x, y);
    probeRaw = 0;
    return r;
}

u8 gg_solid(s16 x, s16 y) __naked
{
    (void)x; (void)y;
    __asm
    ld  c, #0x81
    jp  _probe_common
    __endasm;
}

u8 gg_down_solid(s16 x, s16 y) __naked
{
    (void)x; (void)y;
    __asm
    ld  c, #0x89
    jp  _probe_common
    __endasm;
}

/* ---- multi-point probes (Jazz's collision body is probed 3 points at a time) */
s16 p3x, p3y;   /* used only from asm: keep non-static */

/* Any of (x, y+4), (x, y+11), (x, y+19) solid: Jazz's side probe,
 * point by point (the general case). */
static void side3_slow(void) __naked
{
    __asm
    ld  (_p3x), hl
    ld  (_p3y), de
    ld  hl, #4
    add hl, de
    ex  de, hl
    ld  hl, (_p3x)
    ld  c, #0x81
    call _probe_common
    or  a, a
    ret nz
    ld  hl, (_p3y)
    ld  de, #11
    add hl, de
    ex  de, hl
    ld  hl, (_p3x)
    ld  c, #0x81
    call _probe_common
    or  a, a
    ret nz
    ld  hl, (_p3y)
    ld  de, #19
    add hl, de
    ex  de, hl
    ld  hl, (_p3x)
    ld  c, #0x81
    jp  _probe_common
    __endasm;
}

/* Any of (x+2, y), (x+7, y), (x+11, y): feet (one-way aware) or head,
 * point by point (the general case).  C = table. */
static void row3_slow(void) __naked
{
    __asm
    ld  a, c
    ld  (_p3c), a
    ld  (_p3y), de
    ld  (_p3x), hl
    inc hl
    inc hl
    call _probe_common
    or  a, a
    ret nz
    ld  hl, (_p3x)
    ld  de, #7
    add hl, de
    ld  de, (_p3y)
    ld  a, (_p3c)
    ld  c, a
    call _probe_common
    or  a, a
    ret nz
    ld  hl, (_p3x)
    ld  de, #11
    add hl, de
    ld  de, (_p3y)
    ld  a, (_p3c)
    ld  c, a
    jp  _probe_common
    __endasm;
}
u8 p3c;

/* ---- batched fast paths: when the three points lie in one 32 px block
 * (nearly always), one block lookup and one bank switch answer all three:
 * any point solid <=> OR of their mask bits is set (a destroyed block has
 * no solid points at all). */
u8 s3xlo, s3ylo, s3tab;

/* Cell id of the in-map point (HL = x, DE = y) via the one-entry cache.
 * Out: A = id, B = bx, C = by.  Preserves HL, DE. */
static void block_id(void) __naked
{
    __asm
    ld  a, l
    rlca
    rlca
    rlca
    and a, #7
    ld  b, a
    ld  a, h
    add a, a
    add a, a
    add a, a
    or  a, b
    ld  b, a
    ld  a, e
    rlca
    rlca
    rlca
    and a, #7
    ld  c, a
    ld  a, d
    add a, a
    add a, a
    add a, a
    or  a, c
    ld  c, a
    ld  a, (_lastBx)
    cp  a, b
    jr  nz, 09451$
    ld  a, (_lastBy)
    cp  a, c
    jr  nz, 09451$
    ld  a, (_lastId)
    ret
09451$:
    ld  a, b
    ld  (_lastBx), a
    ld  a, c
    ld  (_lastBy), a
    push hl
    ld  a, (0xFFFF)
    push af
    ld  a, (_gg_cellBank)
    ld  (0xFFFF), a
    ld  a, c
    add a, #0x80
    ld  h, a
    ld  l, b
    ld  a, (hl)
    ld  (_lastId), a
    ld  l, a
    pop af
    ld  (0xFFFF), a
    ld  a, l
    pop hl
    ret
    __endasm;
}

/* Z flag clear (A != 0) when block (B, C) counts as destroyed. */
static void block_destroyed(void) __naked
{
    __asm
    ld  a, (_probeRaw)
    or  a, a
    jr  nz, 09461$
    ld  a, (_game)
    or  a, a
    ret z
    push hl
    push de
    ld  a, b
    ld  l, c
    call _gg_cell_destroyed
    pop de
    pop hl
    or  a, a
    ret
09461$:
    xor a, a
    ret
    __endasm;
}

/* HL -> mask row base for cell id A in table s3tab: hi = tab + (id >> 5),
 * lo = (id << 3) & 0xF8. */
static void mask_base(void) __naked
{
    __asm
    ld  c, a
    rlca
    rlca
    rlca
    and a, #7
    ld  b, a
    ld  a, (_s3tab)
    add a, b
    ld  h, a
    ld  a, c
    add a, a
    add a, a
    add a, a
    ld  l, a
    ret
    __endasm;
}

u8 gg_side3(s16 x, s16 y) __naked
{
    (void)x; (void)y;
    __asm
    ld  a, h
    and a, #0xE0
    jr  z, 09471$
    ld  a, #1
    ret
09471$:
    push de
    ex  de, hl
    ld  bc, #4
    add hl, bc
    ex  de, hl
    ld  a, d
    and a, #0xF8
    jr  nz, 09479$
    ld  a, e
    and a, #31
    cp  a, #17
    jr  nc, 09479$
    pop af
    ld  a, e
    ld  (_s3ylo), a
    ld  a, l
    ld  (_s3xlo), a
    call _block_id
    call _block_destroyed
    jr  z, 09472$
    xor a, a
    ret
09472$:
    ld  a, #0x81
    ld  (_s3tab), a
    ld  a, (_lastId)
    call _mask_base
    ld  a, (0xFFFF)
    ld  b, a
    ld  a, (_gg_miscBank)
    ld  (0xFFFF), a
    ld  c, l
    ld  a, (_s3ylo)
    ld  e, a
    rrca
    rrca
    and a, #7
    or  a, c
    ld  l, a
    ld  d, (hl)
    ld  a, e
    add a, #7
    rrca
    rrca
    and a, #7
    or  a, c
    ld  l, a
    ld  a, (hl)
    or  a, d
    ld  d, a
    ld  a, e
    add a, #15
    rrca
    rrca
    and a, #7
    or  a, c
    ld  l, a
    ld  a, (hl)
    or  a, d
    ld  d, a
    ld  a, b
    ld  (0xFFFF), a
    ld  a, (_s3xlo)
    rrca
    rrca
    and a, #7
    ld  e, a
    ld  hl, #_bitmask
    ld  a, l
    add a, e
    ld  l, a
    jr  nc, 09473$
    inc h
09473$:
    ld  a, (hl)
    and a, d
    ret z
    ld  a, #1
    ret
09479$:
    ; general case: the original y is on the stack, HL = x
    pop de
    jp  _side3_slow
    __endasm;
}

/* (x+2|x+7|x+11, y) with table C. */
static void row3(void) __naked
{
    __asm
    ld  a, c
    ld  (_s3tab), a
    ld  a, d
    and a, #0xF8
    jr  nz, 09489$
    push hl
    inc hl
    inc hl
    ld  a, h
    and a, #0xE0
    jr  nz, 09488$
    ld  a, l
    and a, #31
    cp  a, #23
    jr  nc, 09488$
    ld  a, l
    ld  (_s3xlo), a
    call _block_id
    call _block_destroyed
    jr  z, 09482$
    pop hl
    xor a, a
    ret
09482$:
    pop hl
    ld  a, e
    ld  (_s3ylo), a
    ld  a, (_lastId)
    call _mask_base
    ld  a, (_s3ylo)
    rrca
    rrca
    and a, #7
    or  a, l
    ld  l, a
    ld  a, (0xFFFF)
    ld  b, a
    ld  a, (_gg_miscBank)
    ld  (0xFFFF), a
    ld  d, (hl)
    ld  a, b
    ld  (0xFFFF), a
    ; bits for columns x2, x2+5, x2+9
    ld  a, (_s3xlo)
    ld  c, a
    rrca
    rrca
    and a, #7
    call 09495$
    ld  e, a
    ld  a, c
    add a, #5
    rrca
    rrca
    and a, #7
    call 09495$
    or  a, e
    ld  e, a
    ld  a, c
    add a, #9
    rrca
    rrca
    and a, #7
    call 09495$
    or  a, e
    and a, d
    ret z
    ld  a, #1
    ret
09495$:
    ld  hl, #_bitmask
    add a, l
    ld  l, a
    jr  nc, 09496$
    inc h
09496$:
    ld  a, (hl)
    ret
09488$:
    pop hl
09489$:
    ld  a, (_s3tab)
    ld  c, a
    jp  _row3_slow
    __endasm;
}

u8 gg_feet3(s16 x, s16 y) __naked
{
    (void)x; (void)y;
    __asm
    ld  c, #0x89
    jp  _row3
    __endasm;
}

u8 gg_head3(s16 x, s16 y) __naked
{
    (void)x; (void)y;
    __asm
    ld  c, #0x81
    jp  _row3
    __endasm;
}

/* Distance 0..8 from (x, y) down to the first feet-solid pixel, 9 if none.
 * Solidity is constant over a 4 px mask row, so only the (at most three)
 * rows the 9-pixel span touches are probed: the first solid row starting at
 * pixel r*4 gives distance max(0, r*4 - y). */
u8 gg_ground_dist(s16 x, s16 y)
{
    s16 ry = y & ~3;
    s16 end = y + 8;
    for (; ry <= end; ry += 4) {
        if (gg_down_solid(x, ry)) {
            s16 d = ry - y;
            return (u8)((d < 0) ? 0 : d);
        }
    }
    return 9;
}

u8 scanGx, scanGy0, scanN, scanIds[16];

/* scanIds[0..scanN) = event ids down column scanGx from row scanGy0. */
void gg_scan_column(void) __naked
{
    __asm
    ld  a, (0xFFFF)
    push af
    ld  a, (_gg_eventBank)
    ld  (0xFFFF), a
    ld  a, (_scanGy0)
    add a, #0x80
    ld  h, a
    ld  a, (_scanGx)
    ld  l, a
    ld  de, #_scanIds
    ld  a, (_scanN)
    or  a, a
    jr  z, 09992$
    ld  b, a
09991$:
    ld  a, (hl)
    ld  (de), a
    inc de
    inc h
    djnz 09991$
09992$:
    pop af
    ld  (0xFFFF), a
    ret
    __endasm;
}

/* n event ids down column gx from row gy0 (all inside the map). */
void gg_event_column(u8 gx, u8 gy0, u8 n, u8 *out)
{
    u8 save = SMS_getROMBank();
    const u8 *p;
    u16 off = gy0;
    off <<= 8;
    off |= gx;
    p = (const u8 *)(0x8000 + off);
    SMS_mapROMBank(gg_eventBank);
    while (n--) {
        *out++ = *p;
        p += 256;
    }
    SMS_mapROMBank(save);
}
