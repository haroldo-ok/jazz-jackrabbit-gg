/* Host implementation of the fixed-ROM accessors (gg_world.c), reading the
 * same converted banks the ROM uses (gen/data.bin), with the asm routines'
 * semantics written out in C. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jazz_game.h"
#include "gg_data.h"

Jj1EventInfo gg_einfo[128];
static unsigned char *blob;
static const unsigned char *cellB, *eventB, *miscB;

static const unsigned char *bank(int n) { return blob + (size_t)(n - DATA_BANK0) * 0x4000; }

void host_world_init(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    blob = malloc((size_t)n);
    if (fread(blob, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
}

void gg_world_load(u8 stage)
{
    const GGStageInfo *si = &gg_stage_info[stage];
    cellB = bank(si->cellBank);
    eventB = bank(si->eventBank);
    miscB = bank(si->miscBank);
    memcpy(gg_einfo, miscB + 0x1100, 512);
}

void gg_stage_facts(u8 stage, u8 *sx, u8 *sy, u16 *items, u8 *bossPresent, u8 *bx, u8 *by)
{
    const GGStageInfo *si = &gg_stage_info[stage];
    *sx = si->startX; *sy = si->startY; *items = (u16)si->items;
    *bossPresent = si->bossPresent; *bx = si->bossX; *by = si->bossY;
}

u8 gg_event(s16 bx, s16 by)
{
    if ((u16)bx >= 256 || (u16)by >= 64) return 0;
    return eventB[((u16)by << 8) | (u16)bx];
}

u8 jazz_event_taken(u8 gx, u8 gy)
{
    u16 bit = (u16)(((u16)gy << 8) | gx);
    return (u8)((game.taken[bit >> 3] >> (bit & 7)) & 1);
}

u8 gg_cell_destroyed(u8 gx, u8 gy)
{
    u8 i;
    for (i = 0; i < game.destroyedCount; i++)
        if (game.destroyedX[i] == gx && game.destroyedY[i] == gy) return 1;
    return 0;
}

static u8 probeRaw;
static u8 probe(s16 x, s16 y, unsigned off)
{
    u8 bx, by, id, m;
    if (x < 0 || y < 0 || x >= 8192 || y >= 2048) return 1;
    bx = (u8)(x >> 5); by = (u8)(y >> 5);
    id = cellB[((u16)by << 8) | bx];
    m = miscB[off + ((unsigned)id << 3) + ((y >> 2) & 7)];
    if (!(m & (1 << ((x >> 2) & 7)))) return 0;
    if (!probeRaw && game.destroyedCount && gg_cell_destroyed(bx, by)) return 0;
    return 1;
}
u8 gg_solid(s16 x, s16 y) { return probe(x, y, 0x100); }
u8 gg_down_solid_raw(s16 x, s16 y) { u8 r; probeRaw = 1; r = probe(x, y, 0x900); probeRaw = 0; return r; }
u8 gg_down_solid(s16 x, s16 y) { return probe(x, y, 0x900); }
u8 gg_side3(s16 x, s16 y) { return gg_solid(x, y + 4) || gg_solid(x, y + 11) || gg_solid(x, y + 19); }
u8 gg_feet3(s16 x, s16 y) { return gg_down_solid(x + 2, y) || gg_down_solid(x + 7, y) || gg_down_solid(x + 11, y); }
u8 gg_head3(s16 x, s16 y) { return gg_solid(x + 2, y) || gg_solid(x + 7, y) || gg_solid(x + 11, y); }
u8 gg_ground_dist(s16 x, s16 y)
{
    s16 ry = (s16)(y & ~3), end = (s16)(y + 8);
    for (; ry <= end; ry += 4)
        if (gg_down_solid(x, ry)) { s16 d = (s16)(ry - y); return (u8)(d < 0 ? 0 : d); }
    return 9;
}
void gg_event_column(u8 gx, u8 gy0, u8 n, u8 *out)
{
    while (n--) { *out++ = eventB[((u16)gy0 << 8) | gx]; gy0++; }
}

u8 scanGx, scanGy0, scanN, scanIds[16];
void gg_scan_column(void) { gg_event_column(scanGx, scanGy0, scanN, scanIds); }
