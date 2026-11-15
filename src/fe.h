#ifndef FE_H
#define FE_H
/* Front end: menus, episode selection, info pages, cutscene screens (bank 3,
 * frontend.c) and the bonus levels (bank 4, bonus.c), with their fixed-ROM
 * services (fe_fixed.c).  The banked functions are reached through the
 * trampolines in main.c, which page their bank in first. */
#include "jazz_game.h"

typedef struct {
    u8 music;       /* 1 = on */
    u8 sfx;         /* 1 = on */
    u8 controls;    /* 0: 1 fire, 2 jump;  1: 2 fire, 1 jump */
} FeOptions;
extern FeOptions feOptions;

/* ---- fixed ROM (fe_fixed.c) ---- */
void fe_vram_tiles(u8 bank, u16 addr, u16 bytes, u16 tile);
void fe_nametable(u8 bank, u16 addr, u8 x, u8 y, u8 w, u8 h);
void fe_rom_copy(u8 bank, u16 addr, void *dst, u16 n);
u8 fe_rom_byte(u8 bank, u16 addr);
void fe_cram(u8 index, u16 colour);
void fe_options_load(void);
void fe_options_save(void);
u8 fe_save_valid(u8 slot);
u8 fe_save_write(u8 slot);
u8 fe_save_read(u8 slot);
#define FE_SAVE_SLOTS 4

/* main.c services the banked screens use */
void screen_reset(void);       /* display off, blank nametable, no sprites, no sky */
u16 fe_frame(void);            /* wait for VBlank, return the newly pressed keys */
extern u16 feKeys;             /* keys held after the last fe_frame() */
void play_song(u8 song);       /* music_play honouring the music option */

/* ---- bank 3 (frontend.c) ---- */
/* Stage-select cheat (UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT 2 1 on the title
 * or the main menu): unlocked, and the stage START begins on (stages, then
 * the three bonus levels). */
extern u8 feCheat, feCheatStage;
u8 fe_title(void);                       /* 0 when START is pressed */
u8 fe_main_menu(u8 option);              /* 0..5, 0xFF = back to the title */
u8 fe_episode_menu(u8 row);              /* row 0..10 (10 = bonus levels), 0xFF = back */
u8 fe_slot_menu(u8 saving, u8 message);  /* slot 0..3, 0xFF = back */
void fe_setup_menu(void);
void fe_info(u8 order);
void fe_planet_screen(u8 episode, u8 planet);
void fe_text(u8 kind, u8 episode);       /* FE_TEXT_* */
#define FE_TEXT_LEVEL_CLEAR 0
#define FE_TEXT_BONUS_CLEAR 1
#define FE_TEXT_ENDING 2
#define FE_TEXT_GAME_OVER 3

/* ---- bank 4 (bonus.c) ---- */
#define BONUS_WON 1
#define BONUS_LOST 0xFF
u8 bonus_play(u8 index);

#endif
