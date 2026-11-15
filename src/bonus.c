/* JJ1 bonus levels on the Game Gear (bank 4).
 *
 * The simulation is the Genesis port's bonus_core.c (itself the original's
 * model): positions in 1/1024 px, a turn = 1024 angle units, the same speeds,
 * turning, jumping, crashing, gems, clocks and exit, at 16 ms a step.
 *
 * The view follows the Genesis bonus_view.c at half its resolution: a sky
 * gradient and the horizon strip turning with Jazz (line interrupts in
 * bonus_fixed.c), the perspective floor as 40x10 samples drawn with 256
 * "pair" tiles (left colour | right colour), the items as sprites at four
 * sizes, Jazz's bonus animation (half size) and the gem/time line.  Map and
 * texture reads go through bonus_fixed.c, which pages the data banks in. */

#include <SMSlib.h>
#include "jazz_game.h"
#include "gg_data.h"
#include "fe.h"
#include "bonus_fixed.h"
#include "music.h"
#include "sound.h"

#define F1   1024L
#define FQ   256L
#define FH   512L
#define F12  12288L
#define F16  16384L
#define F20  20480L
#define ITOF(x) ((s32)(x) << 10)
#define MUL(x, y) ((s32)(((s32)(x) * (s32)(y)) >> 10))

#define PRS_CRASH   (-ITOF(60))
#define PRS_REVERSE (-ITOF(30))
#define PRS_JUMP    ITOF(40)
#define PRS_WALK    ITOF(40)
#define PRS_RUN     ITOF(80)
#define PAS_TURN    384
#define PZS_FALL    (-ITOF(2))
#define PZS_JUMP    ITOF(3)
#define PRA_REBOUND 150
#define PRA_REVERSE 450
#define PRA_STOP    200
#define PRA_WALK    100
#define PAA_STOP    6
#define PAA_TURN    1
#define PZA_GRAVITY (-6)
#define MSPS 16

#define IN_UP    0x01
#define IN_DOWN  0x02
#define IN_LEFT  0x04
#define IN_RIGHT 0x08
#define IN_RUN   0x10
#define IN_JUMP  0x20

#define MAX_REMOVED 104

typedef struct {
    const BonusMeta *meta;
    s32 endTime, ticks;
    s32 x, y, z, dz, dr, da, direction;
    u8 running, crashed;
    u16 gems;
    u16 removed[MAX_REMOVED];
    u8 removedCount;
    u8 removedRows[32];
    u8 sound;
} Bonus;

/* The bonus state lives in the renderer's RAM (render.c renderOverlay),
 * which the stage redraw after the bonus level rebuilds. */
extern u8 renderOverlay[];
#define b (*(Bonus *)renderOverlay)

/* sine and cosine (1024 a turn, amplitude 1024) from a quarter table */
static s16 bsin(s32 a)
{
    u16 i = (u16)a & 1023;
    if (i < 256) return bonus_sin[i];
    if (i < 512) return bonus_sin[512 - i];
    if (i < 768) return -bonus_sin[i - 512];
    return -bonus_sin[1024 - i];
}
static s16 bcos(s32 a) { return bsin(a + 256); }

/* width of floor row `row` at heading a (256 a turn): d * cos, 1/8 px */
static s16 side_at(const s16 *row, u8 a)
{
    if (a < 64) return row[a];
    if (a < 128) return -row[128 - a];
    if (a < 192) return -row[a - 128];
    return row[256 - a];
}

static u8 is_removed(u16 cell)
{
    u8 i, row = (u8)(cell >> 8);
    if (!(b.removedRows[row >> 3] & (1 << (row & 7)))) return 0;
    for (i = 0; i < b.removedCount; i++) if (b.removed[i] == cell) return 1;
    return 0;
}

static void remove_cell(u16 cell)
{
    if (b.removedCount < MAX_REMOVED) {
        b.removed[b.removedCount++] = cell;
        b.removedRows[(cell >> 8) >> 3] |= (u8)(1 << ((cell >> 8) & 7));
    }
}

static u8 event_at(u8 tx, u8 ty)
{
    u8 ev = bx_event(tx, ty);
    if (ev && is_removed(((u16)ty << 8) | tx)) return 0;
    return ev;
}

static u8 in_event_area(s32 x, s32 y)
{
    u16 fx = (u16)x & 32767, fy = (u16)y & 32767;
    return (fx > F12) && (fx < F20) && (fy > F12) && (fy < F20);
}

/* (tile, 4 px cell) of a coordinate, from its bytes: tile = bits 15..22,
 * cell = bits 12..14. */
#define TILE_OF(v) ((u8)((((const u8 *)&(v))[1] >> 7) | (((const u8 *)&(v))[2] << 1)))
#define CELL_OF(v) ((u8)((((const u8 *)&(v))[1] >> 4) & 7))

static u8 blocked(s32 x, s32 y)
{
    u8 tx = TILE_OF(x), ty = TILE_OF(y);
    u8 m = bx_probe(tx, ty, CELL_OF(y), CELL_OF(x));
    u8 ev = bxProbeEvent;
    if (ev && is_removed(((u16)ty << 8) | tx)) ev = 0;
    if (ev && b.meta->used[ev & 15] && !b.meta->passable[ev & 15] && in_event_area(x, y)) return 1;
    return m;
}

static void player_step(u8 in)
{
    s32 cdx, cdy;
    if (b.crashed) {
        if (b.dr < 0) b.dr += PRA_REBOUND * MSPS;
        else { b.dr = 0; b.crashed = 0; }
    } else {
        if (in & IN_RUN) { b.running = 1; b.dr = PRS_RUN; }
        if (in & IN_UP) {
            if (b.dr < 0) b.dr += PRA_REVERSE * MSPS;
            else if (b.dr < PRS_WALK) b.dr += PRA_WALK * MSPS;
        } else if (in & IN_DOWN) {
            b.running = 0;
            if (b.dr > 0) b.dr -= PRA_REVERSE * MSPS;
            else if (b.dr > PRS_REVERSE) b.dr -= PRA_WALK * MSPS;
        } else {
            if (!b.running && b.dr > 0) {
                if (b.dr < PRA_STOP * MSPS) b.dr = 0; else b.dr -= PRA_STOP * MSPS;
            }
            if (b.dr < 0) {
                if (b.dr > -PRA_STOP * MSPS) b.dr = 0; else b.dr += PRA_STOP * MSPS;
            }
            if (b.dr == 0) b.running = 0;
        }
        if (in & IN_LEFT) {
            if (b.da > -PAS_TURN) b.da -= PAA_TURN * MSPS;
        } else if (in & IN_RIGHT) {
            if (b.da < PAS_TURN) b.da += PAA_TURN * MSPS;
        } else {
            if (b.da > 0) { if (b.da < PAA_STOP * MSPS) b.da = 0; else b.da -= PAA_STOP * MSPS; }
            if (b.da < 0) { if (b.da > -PAA_STOP * MSPS) b.da = 0; else b.da += PAA_STOP * MSPS; }
        }
        if (b.z == 0 && (in & IN_JUMP)) {
            b.dz = PZS_JUMP;
            if (b.dr < PRS_JUMP) b.dr = PRS_JUMP;
        }
        if (b.dz > PZS_FALL) b.dz += PZA_GRAVITY * MSPS;
    }
    b.direction += (b.da * MSPS) >> 10;
    cdx = (MUL(bsin(b.direction), b.dr) * MSPS) >> 10;
    cdy = (MUL(-bcos(b.direction), b.dr) * MSPS) >> 10;
    if (!blocked(b.x + cdx, b.y)) b.x += cdx;
    if (!blocked(b.x, b.y + cdy)) b.y += cdy;
    b.z += (b.dz * MSPS) >> 10;
    if (b.z > F1) b.z = F1;
    if (b.z < 0) b.z = 0;
    if (b.running && blocked(b.x + cdx, b.y + cdy)) {
        b.running = 0; b.dr = PRS_CRASH; b.da = 0; b.crashed = 1;
    }
}

static void add_time(s16 seconds)
{
    b.endTime += (s32)seconds * 1000;
    if (b.endTime >= b.ticks + 10L * 60 * 1000) b.endTime = b.ticks + 10L * 60 * 1000 - 1;
}

/* 0 continue, BONUS_WON, BONUS_LOST */
static u8 step(u8 input)
{
    s32 px, py;
    u8 tx, ty, ev;
    b.sound = 0;
    b.ticks += MSPS;
    if (b.ticks > b.endTime) { b.sound = 4; return BONUS_LOST; }
    px = b.x; py = b.y;
    player_step(input);
    if (b.z < FH && in_event_area(px, py)) {
        tx = TILE_OF(px); ty = TILE_OF(py);
        ev = event_at(tx, ty);
        if (ev) {
            b.sound = b.meta->sound[ev & 15];
            switch (b.meta->type[ev & 15]) {
            case 1:
                add_time(60);
                remove_cell(((u16)ty << 8) | tx);
                break;
            case 2:
                b.gems++;
                remove_cell(((u16)ty << 8) | tx);
                if (b.gems >= b.meta->items) return BONUS_WON;
                break;
            case 7:
                return BONUS_LOST;
            default:
                break;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------ view */
#define T_FONT    356          /* BG font (palette 0): the HUD's glyphs */
#define T_STRIP   380          /* horizon strip */
static const char hudGlyphs[] = " 0123456789GEMS/:";
#define S_PLAYER  0            /* sprite tiles: 2 x 30 for the player */
#define S_ITEMS   60           /* 4 items x 10 */

#define floorBuf (renderOverlay + 384)

static u8 glyph(char c)
{
    u8 i;
    for (i = 0; hudGlyphs[i]; i++) if (hudGlyphs[i] == c) return i;
    return 0;
}


static void hud(void)
{
    static u16 shownGems = 0xFFFF, shownTime = 0xFFFF;
    char line[17] = "GEMS 00/00  0:00";
    static u8 every;
    u16 t;
    u8 i;
    if (b.gems == shownGems && (++every & 7)) return;
    t = (b.endTime > b.ticks) ? (u16)((u32)(b.endTime - b.ticks) / 1000) : 0;
    if (b.gems == shownGems && t == shownTime) return;
    shownGems = b.gems;
    shownTime = t;
    {
        static u16 words[16];
    line[5] = (char)('0' + (b.gems / 10) % 10);
    line[6] = (char)('0' + b.gems % 10);
    line[8] = (char)('0' + (b.meta->items / 10) % 10);
    line[9] = (char)('0' + b.meta->items % 10);
    line[12] = (char)('0' + (t / 60) % 10);
    line[14] = (char)('0' + (t % 60) / 10);
    line[15] = (char)('0' + t % 10);
        for (i = 0; i < 16; i++) words[i] = T_FONT + glyph(line[i]);
        bx_vram_copy(0, (u16)words, 0x3800 + (3 << 6) + ((6 + 2) << 1), 32);
    }
}

/* the BG font (tiles >= 256 need the nametable's tile bit 8) */
static void hud_reset(void)
{
    u8 i;
    SMS_setNextTileatXY(6, 3);
    for (i = 0; i < 20; i++) SMS_setTile(T_FONT);
}

static u8 fm0, fm1, fm2, fm3, bm0, bm1, bm2, bm3;
static void load_font(u8 fg, u8 bg)
{
    u8 c, y;
    u8 *buf = renderOverlay + 800;
    fm0 = (fg & 1) ? 0xFF : 0; fm1 = (fg & 2) ? 0xFF : 0; fm2 = (fg & 4) ? 0xFF : 0; fm3 = (fg & 8) ? 0xFF : 0;
    bm0 = (bg & 1) ? 0xFF : 0; bm1 = (bg & 2) ? 0xFF : 0; bm2 = (bg & 4) ? 0xFF : 0; bm3 = (bg & 8) ? 0xFF : 0;
    for (c = 0; hudGlyphs[c]; c++) {
        const u8 *src = gg_font + ((u16)gg_font_map[hudGlyphs[c] - 32] << 3);
        u8 *d = buf;
        for (y = 0; y < 8; y++) {
            u8 v = *src++, nv = (u8)~v;
            *d++ = (u8)((v & fm0) | (nv & bm0));
            *d++ = (u8)((v & fm1) | (nv & bm1));
            *d++ = (u8)((v & fm2) | (nv & bm2));
            *d++ = (u8)((v & fm3) | (nv & bm3));
        }
        SMS_loadTiles(buf, T_FONT + c, 32);
    }
}

/* Floor rows (every `stride`-th from `first`): per row the start and the
 * step in 1/2048 px, then the samples (bonus_fixed.c). */
static void render_floor_rows(u8 first, u8 stride)
{
    /* u, v in 1/2048 px (b.x is 1/1024 px); the side table in 1/256 px */
    u8 a = (u8)((u16)b.direction >> 2), sy;
    u32 x8 = (u32)b.x << 1, y8 = (u32)b.y << 1;
    s32 sn8 = (s32)bsin(b.direction) << 5, cs8 = (s32)bcos(b.direction) << 5;
    const s16 *side = bonus_side + first * 65, *stp = bonus_step + first * 65;
    for (sy = first; sy < 10; sy += stride, side += 65 * stride, stp += 65 * stride) {
        s16 s1 = side_at(side, a), s2 = side_at(side, (u8)(a - 64));
        s32 sx = (s32)s1 << 3, syy = (s32)s2 << 3;
        fsU = (u32)(x8 + (syy - sn8) - (sx >> 1));
        fsV = (u32)(y8 - (sx - cs8) - (syy >> 1));
        fsDU = (u32)(s32)side_at(stp, a);           /* (s1 * 8) / 40 samples */
        fsDV = (u32)(s32)side_at(stp, (u8)(a - 64));
        fsOut = floorBuf + sy * 20;
        bx_floor_row();
    }
}

/* num * 1024 / div for 0 < |num| < div: ten steps of long division. */
static s16 qdiv(s32 num, s32 div)
{
    u32 r = (u32)((num < 0) ? -num : num), d = (u32)div;
    u16 q = 0;
    u8 i;
    for (i = 0; i < 10; i++) {
        r <<= 1;
        q <<= 1;
        if (r >= d) { r -= d; q |= 1; }
    }
    return (num < 0) ? -(s16)q : (s16)q;
}

/* Items in view, nearest drawn first (on top). */
typedef struct { s16 x; u8 item, size; s32 div; } Seen;
#define SEEN_MAX 14
#define seen ((Seen *)(renderOverlay + 584))

static void draw_items(void)
{
    const s16 sn = bsin(b.direction), cs = bcos(b.direction);
    const s32 cx = 16384 - (b.x & 32767), cy = 16384 - (b.y & 32767);
    const s32 axs = MUL(cx, sn), axc = MUL(cx, cs), ays = MUL(cy, sn), ayc = MUL(cy, cs);
    const u8 bx = (u8)(b.x >> 15), by = (u8)(b.y >> 15);
    u8 n = 0, i, j, k, ev[24];
    s8 dy;
    for (dy = -6; dy <= 6; dy++) {
        u8 ty = (u8)(by + dy);
        u8 cnt = bx_row_events(ty, (u8)(bx - 6), 13, ev, 12);
        for (k = 0; k < cnt; k++) {
            s16 evX = (s16)ev[2 * k] - 6, evY = dy;
            u8 id = ev[2 * k + 1], kind;
            s32 div, num, an;
            if (is_removed(((u16)ty << 8) | (u8)(bx + evX))) continue;
            kind = b.meta->type[id & 15];
            if (!kind || kind == 5) continue;
            {
                s32 t1 = (s32)evX * sn, t2 = (s32)evY * cs;
                div = 16384L + (t1 << 5) + axs - (t2 << 5) - ayc;
            }
            if (div < 9216) continue;
            {
                s32 t3 = (s32)evX * cs, t4 = (s32)evY * sn;
                num = (t3 << 5) + axc + (t4 << 5) + ays;
            }
            an = (num < 0) ? -num : num;
            if ((an << 2) >= div + div + div) continue;
            if (n >= SEEN_MAX) continue;
            {
                s16 nX = qdiv(num, div);
                s16 scr = (s16)(((s32)nX * 160) >> 10) + 80;
                u8 size = (div <= 56157L) ? 0 : (div <= 78582L) ? 1 : (div <= 131072L) ? 2 : 3;
                if (scr > -20 && scr < 180) {
                    seen[n].x = scr;
                    seen[n].item = (kind == 1) ? 0 : (kind == 2) ? 1 : (kind == 8) ? 2 : 3;
                    seen[n].size = size;
                    seen[n].div = div;
                    n++;
                }
            }
        }
    }
    for (i = 1; i < n; i++) {
        Seen t;
        t = seen[i];
        for (j = i; j > 0 && seen[j - 1].div > t.div; j--) seen[j] = seen[j - 1];
        seen[j] = t;
    }
    for (i = 0; i < n; i++) {
        static const u8 px[4] = { 16, 12, 8, 4 };
        static const u8 off[4] = { 0, 4, 8, 9 };
        u8 s = seen[i].size;
        u8 tile = (u8)(S_ITEMS + seen[i].item * 10 + off[s]);
        s16 x = 48 + seen[i].x - (px[s] >> 1);
        s16 y = 24 + 60 - (px[s] >> 1);
        if (s < 2) {
            u8 bx2 = (u8)(px[s] == 16 ? 0 : 2);
            SMS_addSpriteClipping(x - bx2, y - bx2, tile);
            SMS_addSpriteClipping(x - bx2 + 8, y - bx2, tile + 1);
            SMS_addSpriteClipping(x - bx2, y - bx2 + 8, tile + 2);
            SMS_addSpriteClipping(x - bx2 + 8, y - bx2 + 8, tile + 3);
        } else {
            SMS_addSpriteClipping(x - (s == 2 ? 0 : 2), y - (s == 2 ? 0 : 2), tile);
        }
    }
}

/* Player frames stream into one of two 30-tile buffers, 6 tiles a frame,
 * and the sprites switch over when the new frame is complete. */
static u8 shownFrame = 0xFF, loadingFrame = 0xFF, loadStep, frontBuf;

static void player_upload(void)
{
    if (loadingFrame == 0xFF) return;
    {
        const BonusFrame *f = &bonus_player_frame[loadingFrame];
        u8 back = frontBuf ^ 1;
        bx_vram_copy(f->bank, f->addr + loadStep * (6 * 32),
                     (u16)(256 + S_PLAYER + back * 30 + loadStep * 6) << 5, 6 * 32);
        if (++loadStep == 5) {
            frontBuf = back;
            shownFrame = loadingFrame;
            loadingFrame = 0xFF;
        }
    }
}

static void draw_player(s16 lift)
{
    u8 r, c, t = (u8)(S_PLAYER + frontBuf * 30);
    s16 x0 = 48 + 80 - 20, y0 = 24 + 136 - 48 - lift;
    if (shownFrame == 0xFF) return;
    for (r = 0; r < 6; r++)
        for (c = 0; c < 5; c++, t++)
            SMS_addSpriteClipping(x0 + c * 8, y0 + r * 8, t);
}

static u8 bonus_keys(u16 k)
{
    u8 in = 0;
    u16 fireKey = feOptions.controls ? PORT_A_KEY_2 : PORT_A_KEY_1;
    u16 jumpKey = feOptions.controls ? PORT_A_KEY_1 : PORT_A_KEY_2;
    if (k & PORT_A_KEY_UP) in |= IN_UP;
    if (k & PORT_A_KEY_DOWN) in |= IN_DOWN;
    if (k & PORT_A_KEY_LEFT) in |= IN_LEFT;
    if (k & PORT_A_KEY_RIGHT) in |= IN_RIGHT;
    if (k & fireKey) in |= IN_RUN;
    if (k & jumpKey) in |= IN_JUMP;
    return in;
}

extern volatile u8 vblankTicks;
extern u8 bxBand, bxStripScroll;
extern u16 bxSky[6];
void bx_line_irq(void);
void bx_set_irqs(u8 on);

static u16 whiten(u16 c, u8 lvl)
{
    u8 r = c & 15, g = (c >> 4) & 15, bl = (c >> 8) & 15;
    r += (u8)(((15 - r) * lvl) >> 4);
    g += (u8)(((15 - g) * lvl) >> 4);
    bl += (u8)(((15 - bl) * lvl) >> 4);
    return (u16)(r | (g << 4) | (bl << 8));
}

u8 bonus_play(u8 index)
{
    const BonusMeta *m;
    u8 result = 0, i, last, frame = 0, white = 0, animMs = 0, animPos = 0;
    u16 keys = 0;
    if (index > 2) index = 0;
    m = &bonus_meta[index];
    b.meta = m;
    bxBank = m->bank;
    bxRefs = m->refs;
    bxChunks = m->chunks;
    bxMasks = m->masks;
    bxRows = m->evrows;
    b.endTime = (s32)m->seconds * 1000;
    b.ticks = MSPS;
    b.x = ((s32)m->startX << 15) + F16;
    b.y = ((s32)m->startY << 15) + F16;
    b.z = b.dz = b.dr = b.da = 0;
    b.direction = FQ;
    b.running = b.crashed = 0;
    b.gems = 0;
    b.removedCount = 0;
    for (i = 0; i < 32; i++) b.removedRows[i] = 0;
    shownFrame = loadingFrame = 0xFF;
    frontBuf = 0;
    for (i = 0; i < 6; i++) bxSky[i] = bonus_sky[i];

    /* display */
    screen_reset();
    GG_loadBGPalette(bonus_floor_pal);
    GG_loadSpritePalette(bonus_sprite_pal);
    {   /* the 256 pair tiles: left 4 px colour a, right 4 px colour b */
        u8 buf[32], a2, b2, y, k;
        for (a2 = 0; a2 < 16; a2++)
            for (b2 = 0; b2 < 16; b2++) {
                for (k = 0; k < 4; k++) {
                    u8 p = (u8)(((a2 >> k) & 1 ? 0xF0 : 0) | ((b2 >> k) & 1 ? 0x0F : 0));
                    for (y = 0; y < 8; y++) buf[y * 4 + k] = p;
                }
                SMS_loadTiles(buf, (a2 << 4) | b2, 32);
            }
    }
    {
        u8 hi = 1, lo = 1;
        u16 best = 0, worst = 0xFFFF;
        for (i = 1; i < 16; i++) {
            u16 c = bonus_floor_pal[i], v = (u16)((c & 15) * 3 + ((c >> 4) & 15) * 6 + ((c >> 8) & 15));
            if (v > best) { best = v; hi = i; }
            if (v < worst) { worst = v; lo = i; }
        }
        load_font(hi, lo);
    }
    fe_vram_tiles(BONUS_STRIP_TILES_BANK, BONUS_STRIP_TILES_ADDR, BONUS_STRIP_TILES_SIZE, T_STRIP);
    fe_vram_tiles(BONUS_ITEM_TILES_BANK, BONUS_ITEM_TILES_ADDR, BONUS_ITEM_TILES_SIZE, 256 + S_ITEMS);
    /* HUD row, sky (colour 0), strip at rows 6-7 */
    hud_reset();
    for (i = 0; i < 2; i++) {
        u8 x;
        SMS_setNextTileatXY(0, 3 + 6 + i);
        for (x = 0; x < 32; x++) SMS_setTile(T_STRIP + bonus_strip_map[i * 32 + x]);
    }
    /* the first player frame */
    {
        const BonusFrame *f = &bonus_player_frame[bonus_anim[index][6][0]];
        fe_vram_tiles(f->bank, f->addr, 30 * 32, 256 + S_PLAYER);
        shownFrame = bonus_anim[index][6][0];
    }
    bx_floor_reset();
    render_floor_rows(0, 1);
    hud();
    bx_floor_upload(floorBuf);
    bx_set_irqs(1);
    SMS_displayOn();
    play_song(SONG_BONUS);
    last = vblankTicks;

    for (;;) {
        u8 steps, k;
        s16 lift;
        /* ---- simulation: one 16 ms step per elapsed video frame */
        steps = (u8)(vblankTicks - last);
        last += steps;
        if (!steps) steps = 1;
        if (steps > 8) steps = 8;
        if (!result) {
            for (k = 0; k < steps; k++) {
                result = step(bonus_keys(keys));
                if (b.sound == 3) sound_play(SFX_GODLIKE, 2);
                else if (b.sound == 4) sound_play(SFX_OW, 2);
                if (result) break;
            }
        } else {
            /* the end: spin and fade to white over two seconds */
            b.direction += 12 + (white >> 1);
            white = (u8)(white + steps);
            if (white >= 120) break;
        }
        frame++;
        /* ---- animation (OpenJazz's slot per state, 75 ms a frame) */
        {
            u8 anim = b.crashed ? 5 : b.z > 0 ? 4 :
                      (keys & PORT_A_KEY_LEFT) ? 1 : (keys & PORT_A_KEY_RIGHT) ? 2 :
                      (keys & (PORT_A_KEY_UP | PORT_A_KEY_DOWN)) ? 0 : 6;
            u8 cnt = bonus_anim_count[index][anim];
            u8 which;
            animMs += (u8)(steps << 4);
            while (animMs >= 75) { animMs -= 75; animPos++; }
            while (animPos >= 19 * 7) animPos -= 19 * 7;
            {
                u8 p = animPos;
                while (p >= cnt) p -= cnt;
                which = bonus_anim[index][anim][p];
            }
            if (which != shownFrame && loadingFrame == 0xFF) { loadingFrame = which; loadStep = 0; }
        }
        /* the floor's rows alternate between passes: each pass draws half */
        render_floor_rows((u8)(frame & 1), 2);
        bxStripScroll = (u8)(-(s16)(b.direction >> 1));
        lift = (s16)((b.z * 40) >> 10);
        SMS_initSprites();
        draw_player(lift);
        draw_items();
        SMS_finalizeSprites();

        /* a fresh VBlank, so the VRAM traffic below ends before the
           raster interrupts start (line 31) */
        {
            u8 t = vblankTicks;
            while (vblankTicks == t) { }
        }
        bx_sat();
        __asm__("di");
        bx_floor_upload(floorBuf);
        __asm__("ei");
        hud();
        player_upload();
        if (result) {
            u8 lvl = (u8)((white * 16) / 120);
            for (i = 0; i < 16; i++) {
                fe_cram(i, whiten(bonus_floor_pal[i], lvl));
                fe_cram((u8)(16 + i), whiten(bonus_sprite_pal[i], lvl));
            }
            for (i = 0; i < 6; i++) bxSky[i] = whiten(bonus_sky[i], lvl);
        }
        keys = SMS_getKeysStatus();
    }
    bx_set_irqs(0);
    sound_silence();
    play_song(0xFF);
    return result;
}
