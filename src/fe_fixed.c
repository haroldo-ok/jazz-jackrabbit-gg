/* Fixed-ROM services for the banked front end (frontend.c, bank 3) and the
 * bonus levels (bonus.c, bank 4).
 *
 * Banked code runs from the slot-2 window, which is also where the data banks
 * and the cartridge RAM appear, so it never touches those itself: these
 * helpers page the data in, copy what is needed and put the caller's bank
 * back.  The save slots live in the cartridge RAM (battery-backed, 16 KB). */

#include <SMSlib.h>
#include "jazz_game.h"
#include "gg_data.h"
#include "fe.h"

__sfr __at 0xBF FeVDPCtrl;
__sfr __at 0xBE FeVDPData;

FeOptions feOptions = { 1, 1, 0 };

/* `bytes` of tile data from ROM (bank, addr) to VRAM tile `tile`. */
void fe_vram_tiles(u8 bank, u16 addr, u16 bytes, u16 tile)
{
    u8 save = SMS_getROMBank();
    SMS_mapROMBank(bank);
    SMS_loadTiles((const void *)addr, tile, bytes);
    SMS_mapROMBank(save);
}

/* A w x h block of nametable words from ROM to the visible Game Gear window
 * at (x, y) (0..19, 0..17). */
void fe_nametable(u8 bank, u16 addr, u8 x, u8 y, u8 w, u8 h)
{
    u8 save = SMS_getROMBank();
    const unsigned int *src = (const unsigned int *)addr;
    u8 i, j;
    SMS_mapROMBank(bank);
    for (j = 0; j < h; j++) {
        SMS_setNextTileatXY(6 + x, 3 + y + j);
        for (i = 0; i < w; i++) SMS_setTile(*src++);
    }
    SMS_mapROMBank(save);
}

/* n bytes from ROM (bank, addr) to RAM. */
void fe_rom_copy(u8 bank, u16 addr, void *dst, u16 n)
{
    u8 save = SMS_getROMBank();
    const u8 *s = (const u8 *)addr;
    u8 *d = (u8 *)dst;
    SMS_mapROMBank(bank);
    while (n--) *d++ = *s++;
    SMS_mapROMBank(save);
}

u8 fe_rom_byte(u8 bank, u16 addr)
{
    u8 save = SMS_getROMBank(), v;
    SMS_mapROMBank(bank);
    v = *(const u8 *)addr;
    SMS_mapROMBank(save);
    return v;
}

/* One CRAM entry, interrupts off (the sky gradient's line interrupt writes
 * CRAM too). */
void fe_cram(u8 index, u16 colour)
{
    __asm__("di");
    FeVDPCtrl = (u8)(index << 1);
    FeVDPCtrl = 0xC0;
    FeVDPData = (u8)colour;
    FeVDPData = (u8)(colour >> 8);
    __asm__("ei");
}

/* ------------------------------------------------------------ saves */
/* Cartridge RAM layout:
 *   0x0000  "JJGG", version, music, sfx, controls, checksum
 *   0x0100 + n * SAVE_STRIDE  slot n: "SAVE", checksum lo, hi, episode, JazzGame */
#define SAVE_STRIDE 0x0C00
#define SAVE_HEAD 7
typedef char save_fits[(SAVE_HEAD + sizeof(JazzGame) <= SAVE_STRIDE) ? 1 : -1];
typedef char slots_fit[(0x100 + 4 * SAVE_STRIDE <= 0x4000) ? 1 : -1];

static u16 sum_bytes(const u8 *p, u16 n)
{
    u16 s = 0x5A5A;
    while (n--) { s = (u16)((s << 1) | (s >> 15)); s += *p++; }
    return s;
}

/* Interrupts stay off while the cartridge RAM is in slot 2: the frame
 * interrupt pages songs in and out of that window. */
void fe_options_load(void)
{
    u8 save = SMS_getROMBank();
    __asm__("di");
    SMS_enableSRAM();
    if (SMS_SRAM[0] == 'J' && SMS_SRAM[1] == 'J' && SMS_SRAM[2] == 'G' && SMS_SRAM[3] == 'G' &&
        SMS_SRAM[4] == 1 && (u8)(SMS_SRAM[5] + SMS_SRAM[6] + SMS_SRAM[7] + 0x33) == SMS_SRAM[8]) {
        feOptions.music = SMS_SRAM[5] & 1;
        feOptions.sfx = SMS_SRAM[6] & 1;
        feOptions.controls = SMS_SRAM[7] & 1;
    }
    SMS_disableSRAM();
    SMS_mapROMBank(save);
    __asm__("ei");
}

void fe_options_save(void)
{
    u8 save = SMS_getROMBank();
    __asm__("di");
    SMS_enableSRAM();
    SMS_SRAM[0] = 'J'; SMS_SRAM[1] = 'J'; SMS_SRAM[2] = 'G'; SMS_SRAM[3] = 'G';
    SMS_SRAM[4] = 1;
    SMS_SRAM[5] = feOptions.music;
    SMS_SRAM[6] = feOptions.sfx;
    SMS_SRAM[7] = feOptions.controls;
    SMS_SRAM[8] = (u8)(feOptions.music + feOptions.sfx + feOptions.controls + 0x33);
    SMS_disableSRAM();
    SMS_mapROMBank(save);
    __asm__("ei");
}

u8 fe_save_valid(u8 slot)
{
    u8 save = SMS_getROMBank(), ok = 0;
    u8 *p;
    __asm__("di");
    SMS_enableSRAM();
    p = &SMS_SRAM[0x100 + (u16)slot * SAVE_STRIDE];
    if (p[0] == 'S' && p[1] == 'A' && p[2] == 'V' && p[3] == 'E' && p[6] < GG_EPISODES) {
        u16 s = sum_bytes(p + SAVE_HEAD, sizeof(JazzGame));
        ok = (u8)(((u8)s == p[4]) && ((u8)(s >> 8) == p[5]));
    }
    SMS_disableSRAM();
    SMS_mapROMBank(save);
    __asm__("ei");
    return ok;
}

u8 fe_save_write(u8 slot)
{
    u8 save = SMS_getROMBank();
    u8 *p;
    u16 s;
    __asm__("di");
    SMS_enableSRAM();
    p = &SMS_SRAM[0x100 + (u16)slot * SAVE_STRIDE];
    {
        const u8 *src = (const u8 *)&game;
        u8 *d = p + SAVE_HEAD;
        u16 n = sizeof(JazzGame);
        while (n--) *d++ = *src++;
    }
    s = sum_bytes(p + SAVE_HEAD, sizeof(JazzGame));
    p[0] = 'S'; p[1] = 'A'; p[2] = 'V'; p[3] = 'E';
    p[4] = (u8)s; p[5] = (u8)(s >> 8);
    p[6] = game.episode;
    SMS_disableSRAM();
    SMS_mapROMBank(save);
    __asm__("ei");
    return fe_save_valid(slot);
}

u8 fe_save_read(u8 slot)
{
    u8 save, *p;
    if (!fe_save_valid(slot)) return 0;
    save = SMS_getROMBank();
    __asm__("di");
    SMS_enableSRAM();
    p = &SMS_SRAM[0x100 + (u16)slot * SAVE_STRIDE];
    {
        u8 *d = (u8 *)&game;
        const u8 *src = p + SAVE_HEAD;
        u16 n = sizeof(JazzGame);
        while (n--) *d++ = *src++;
    }
    SMS_disableSRAM();
    SMS_mapROMBank(save);
    __asm__("ei");
    return 1;
}
