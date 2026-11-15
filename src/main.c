/* Jazz Jackrabbit - Game Gear port of the Genesis/SGDK homebrew port.
 *
 * Fixed ROM: this loop, the renderer, the banked-data accessors, SMSlib.
 * Bank LOGIC_BANK: the game simulation (gg_logic.c), entered through the
 * small trampolines below which page it into the slot-2 window. */

#include <SMSlib.h>
#include "jazz_game.h"
#include "gg_data.h"
#include "render.h"
#include "sound.h"
#include "music.h"
#include "fe.h"

#define LOGIC_BANK 2
#define FE_BANK 3
#define BONUS_BANK 4

SMS_EMBED_SEGA_ROM_HEADER(9999, 0);
SMS_EMBED_SDSC_HEADER_AUTO_DATE(0, 1, "haroldo-ok / Claude", "Jazz Jackrabbit GG",
                                "Game Gear port of the Genesis JJ1 homebrew port");

JazzGame game;

enum { MODE_TITLE, MODE_PLAY, MODE_END };
u8 mode;
u8 selectedStage, selectedEpisode;
u8 menuOption, episodeRow;
u16 keys, prevKeys;
u16 playInput;
volatile u8 vblankTicks;   /* bumped by the frame interrupt */
u8 lastTick;
u8 stepsRun;               /* game frames run by the last loop pass */
u8 benchMode;
u8 stepHist[8];            /* passes by video frames elapsed (profiling) */
#ifdef GG_TIMERS
__sfr __at 0x06 TPort2;
#define TS(i) (TPort2 = (u8)(((i) << 1) | 1))
#define TE(i) (TPort2 = (u8)((i) << 1))
#else
#define TS(i)
#define TE(i)
#endif
#ifdef PASS_TRACE
u8 stepHistLast;
__sfr __at 0x04 PassPort;
#endif

/* Sky gradient: the Genesis draws a banded blue sky (its gfx.c sky palette)
 * on plane B behind every level.  The GG has one background layer, so the
 * transparent colour 0 is recoloured down the screen from the line
 * interrupt: six 24-line bands, the first set just above the visible window
 * (SMS line 23).  The VDP reloads its line counter when it fires, before the
 * handler runs, so writing 255 to register 10 in the fifth band's handler
 * makes the sixth band the last interrupt of the frame. */
__sfr __at 0xBF VDPCtrl;
__sfr __at 0xBE VDPData;
const u16 skyBands[6] = { 0xFDD, 0xF99, 0xF44, 0xB00, 0x600, 0x400 };
u8 skyBand;
u8 skyOn;

static void line_irq(void) __naked
{
    __asm
    ld  a, (_skyBand)
    inc a
    ld  (_skyBand), a
    cp  a, #4
    jr  nz, 09951$
    ld  a, #255
    out (0xBF), a
    ld  a, #0x8A
    out (0xBF), a
    ld  a, #4
09951$:
    cp  a, #6
    ret nc
    add a, a
    ld  e, a
    ld  d, #0
    ld  hl, #_skyBands
    add hl, de
    xor a, a
    out (0xBF), a
    ld  a, #0xC0
    out (0xBF), a
    ld  a, (hl)
    out (0xBE), a
    inc hl
    ld  a, (hl)
    out (0xBE), a
    ret
    __endasm;
}

u8 bonusRaster;
void bx_frame_irq(void);
void bx_line_irq(void);

static void frame_irq(void)
{
    vblankTicks++;
    if (bonusRaster) bx_frame_irq();
    else if (skyOn) {
        skyBand = 0xFF;
        VDPCtrl = 23;       /* line counter period for the next frame */
        VDPCtrl = 0x8A;
    }
    music_frame();
    sound_update();
}

static void sky_enable(u8 on)
{
    skyOn = on;
    if (on) {
        skyBand = 0xFF;
        SMS_setLineInterruptHandler(line_irq);
        SMS_setLineCounter(23);
        SMS_enableLineInterrupt();
    } else {
        SMS_disableLineInterrupt();
    }
}

/* The bonus levels' raster effects (bonus_fixed.c). */
void bx_set_irqs(u8 on)
{
    if (on) {
        skyOn = 0;
        bonusRaster = 1;
        SMS_setLineInterruptHandler(bx_line_irq);
        SMS_setLineCounter(7);
        SMS_enableLineInterrupt();
    } else {
        SMS_disableLineInterrupt();
        bonusRaster = 0;
    }
}

/* ---- trampolines into the banked code ---- */
static void logic_init(void) { SMS_mapROMBank(LOGIC_BANK); jazz_game_init(); }
static void logic_step(u16 in) { SMS_mapROMBank(LOGIC_BANK); jazz_step(in); }
static void logic_set_stage(u8 s) { SMS_mapROMBank(LOGIC_BANK); jazz_set_stage(s); }
static void logic_resume(void) { SMS_mapROMBank(LOGIC_BANK); jazz_resume(); }

#define FE(call) (SMS_mapROMBank(FE_BANK), (call))
static u8 t_title(void) { return FE(fe_title()); }
static u8 t_main_menu(u8 o) { return FE(fe_main_menu(o)); }
static u8 t_episode_menu(u8 r) { return FE(fe_episode_menu(r)); }
static u8 t_slot_menu(u8 s, u8 m) { return FE(fe_slot_menu(s, m)); }
static void t_setup(void) { SMS_mapROMBank(FE_BANK); fe_setup_menu(); }
static void t_info(u8 o) { SMS_mapROMBank(FE_BANK); fe_info(o); }
static void t_planet(u8 e, u8 p) { SMS_mapROMBank(FE_BANK); fe_planet_screen(e, p); }
static void t_text(u8 k, u8 e) { SMS_mapROMBank(FE_BANK); fe_text(k, e); }
static u8 t_bonus(u8 i) { SMS_mapROMBank(BONUS_BANK); return bonus_play(i); }

/* ---- services for the banked screens ---- */
u16 feKeys;

/* Wait for VBlank, then the keys newly pressed since the last call. */
u16 fe_frame(void)
{
    u16 prev = feKeys;
    SMS_waitForVBlank();
    feKeys = SMS_getKeysStatus();
    return feKeys & ~prev;
}

static u8 songPlaying = 0xFF;

/* Start a song (SONG_*, 0xFF = silence) unless it is already playing; with
 * the music switched off in the setup, silence. */
void play_song(u8 song)
{
    if (!feOptions.music) song = 0xFF;
    if (song == songPlaying) return;
    songPlaying = song;
    music_play(song);
}

void screen_reset(void)
{
    sky_enable(0);
    SMS_displayOff();
    SMS_VRAMmemsetW(0x3800, 0, 32 * 28 * 2);
    SMS_initSprites();
    SMS_finalizeSprites();
    SMS_copySpritestoSAT();
    SMS_setBGScrollX(0);
    SMS_setBGScrollY(0);
}

static void stage_art(void)
{
    play_song(gg_stage_info[gg_stageBase + game.stage].song);
    sky_enable(0);
    SMS_displayOff();
    render_stage_begin();
    render_frame(1);
    SMS_waitForVBlank();
    render_vblank();
    SMS_displayOn();
    sky_enable(1);
    lastTick = vblankTicks;
}

static void set_episode(u8 e)
{
    selectedEpisode = e;
    gg_stageBase = gg_episode_info[e].firstStage;
    gg_stageCount = gg_episode_info[e].stages;
}

/* The game keys: 2 jumps and 1 fires, or the other way round (setup);
 * up + fire cycles the weapons (the Genesis C button), except on the
 * airboard, where up is thrust. */
static u16 input_from_keys(u16 k)
{
    u16 in = 0;
    u16 fireKey = feOptions.controls ? PORT_A_KEY_2 : PORT_A_KEY_1;
    u16 jumpKey = feOptions.controls ? PORT_A_KEY_1 : PORT_A_KEY_2;
    if (k & PORT_A_KEY_LEFT) in |= JAZZ_INPUT_LEFT;
    if (k & PORT_A_KEY_RIGHT) in |= JAZZ_INPUT_RIGHT;
    if (k & PORT_A_KEY_UP) in |= JAZZ_INPUT_UP;
    if (k & PORT_A_KEY_DOWN) in |= JAZZ_INPUT_DOWN;
    if (k & jumpKey) in |= JAZZ_INPUT_JUMP;
    if (k & fireKey) {
        if ((k & PORT_A_KEY_UP) && !game.player.flying) in |= JAZZ_INPUT_SWITCH;
        else in |= JAZZ_INPUT_FIRE;
    }
    if (k & GG_KEY_START) in |= JAZZ_INPUT_START;
    return in;
}

/* ---- the planet approach, shown when the planet changes ---- */
static u8 shownPlanet;

static void approach(void)
{
    u8 p = gg_stage_info[gg_stageBase + game.stage].planet;
    if (p == 0xFF || p == shownPlanet) return;
    shownPlanet = p;
    t_planet(game.episode, p);
}

/* A bonus level from the game: winning gives a life. */
static void run_bonus(u8 index)
{
    sound_silence();
    if (t_bonus(index) == BONUS_WON) {
        if (game.lives < 9) game.lives++;
        t_text(FE_TEXT_BONUS_CLEAR, game.episode);
    }
    songPlaying = 0xFE;
}

/* Play until the game ends (episode complete or game over). */
static void play(void)
{
    u16 prevPad;
    shownPlanet = 0xFF;
    approach();
    game.stageChanged = 0;
    game.previousInput = JAZZ_INPUT_START;
    songPlaying = 0xFE;
    stage_art();
    feKeys = SMS_getKeysStatus();
    prevPad = feKeys;
    mode = 1;   /* playing (read by the test scripts) */
    for (;;) {
        u8 n, k, prevStage;
        u16 ev = 0, pad, pressed;
        u8 sfxEv = 0;
        SMS_waitForVBlank();
        render_vblank();
        pad = SMS_getKeysStatus();
        pressed = pad & ~prevPad;
        prevPad = pad;
        /* paused: 1 opens the save slots (the Genesis C button) */
        if (game.paused && (pressed & PORT_A_KEY_1)) {
            u8 slot, msg = 0;
            sound_silence();
            for (;;) {
                slot = t_slot_menu(1, msg);
                if (slot == 0xFF || fe_save_write(slot)) break;
                msg = 1;
            }
            songPlaying = 0xFE;
            stage_art();
            prevPad = SMS_getKeysStatus();
            game.previousInput = input_from_keys(prevPad);
            music_pause(1);
            continue;
        }
        n = (u8)(vblankTicks - lastTick);
        lastTick += n;
        stepHist[(n > 7) ? 7 : n]++;
        if (!n || benchMode) n = 1;
        if (n > 6) n = 6;
        playInput = input_from_keys(pad);
        stepsRun = 0;
        prevStage = game.stage;
        for (k = 0; k < n; k++) {
            TS(6);
            logic_step(playInput);
            TE(6);
            stepsRun++;
            ev |= game.events;
            if (game.sfxEvent) sfxEv = game.sfxEvent;
            if (game.stageChanged || game.finished) break;
        }
        sound_events(ev, sfxEv);
        music_pause(game.paused);
        if (game.stageChanged || game.finished) {
            /* the level was completed (or the last life lost) */
            mode = 0;
            if (!game.finished || game.lives) {
                if (gg_stage_info[gg_stageBase + prevStage].clearScene) {
                    sound_silence();
                    t_text(FE_TEXT_LEVEL_CLEAR, game.episode);
                    songPlaying = 0xFE;
                }
                if (game.bonusPending) {
                    game.bonusPending = 0;
                    run_bonus(gg_stage_info[gg_stageBase + prevStage].bonus);
                }
            }
            if (game.finished) {
                sound_silence();
                if (game.lives) t_text(FE_TEXT_ENDING, game.episode);
                else t_text(FE_TEXT_GAME_OVER, game.episode);
                songPlaying = 0xFE;
                return;
            }
            game.stageChanged = 0;
            approach();
            songPlaying = 0xFE;
            stage_art();
            prevPad = SMS_getKeysStatus();
            game.previousInput = input_from_keys(prevPad);
            mode = 1;
        } else {
            TS(7);
            render_frame(stepsRun);
            TE(7);
        }
    }
}

/* New game in episode `e` (index into gg_episode_info). */
static void new_game(u8 e)
{
    u8 st = 0;
    set_episode(e);
    sound_silence();
    logic_init();
    game.episode = e;
    if (feCheat && feCheatStage) {
        st = feCheatStage;
        if (st >= gg_stageCount) st = (u8)(gg_stageCount - 1);
        logic_set_stage(st);
    } else if (selectedStage < gg_stageCount && selectedStage) {
        logic_set_stage(selectedStage);   /* poked by the test scripts */
    }
    play();
}

/* The bonus levels on their own (episode row Z, or the stage-select cheat). */
static void bonus_alone(u8 index)
{
    set_episode(0);
    logic_init();
    game.episode = 0;
    run_bonus(index);
}

static u8 row_episode(u8 row)
{
    u8 i;
    for (i = 0; i < GG_EPISODES; i++)
        if (gg_episode_info[i].number == row + 1) return i;
    return 0xFF;
}

static void main_menu(void)
{
    for (;;) {
        u8 o = t_main_menu(menuOption);
        if (o == 0xFF || o == 5) return;
        menuOption = o;
        if (o == 0) {
            u8 row = t_episode_menu(episodeRow);
            u8 e;
            if (row == 0xFF) continue;
            episodeRow = row;
            if (feCheat && feCheatStage >= JAZZ_STAGE_COUNT) {
                bonus_alone((u8)(feCheatStage - JAZZ_STAGE_COUNT));
                return;
            }
            if (row == FE_EP_ROWS - 1) { bonus_alone(0); return; }
            e = row_episode(row);
            if (e == 0xFF) continue;
            new_game(e);
            return;
        } else if (o == 1) {
            u8 slot = t_slot_menu(0, 0);
            if (slot == 0xFF || !fe_save_read(slot)) continue;
            set_episode(game.episode);
            logic_resume();
            play();
            return;
        } else if (o == 3) {
            t_setup();
        } else {
            t_info(o == 4);
        }
    }
}

#ifdef GG_TRACE
/* Differential-test build: no title, generated inputs, one game frame per
 * pass, and a DIFF_REC record per frame on debug port 0x04 (smstest writes
 * it to $SMSTEST_TRACE).  Compare with tests/diff/run.sh's host records. */
#include "../tests/diff/diff_common.h"
__sfr __at 0x04 TracePort;
u8 traceStage;
u16 traceFrames = 1500;
u16 traceSeed;

static void trace_main(void)
{
    u16 f;
    for (f = 0; f < 10; f++) SMS_waitForVBlank();   /* tests poke the settings now */
    selectedStage = traceStage;
    if (selectedEpisode >= GG_EPISODES) selectedEpisode = 0;
    if (traceSeed) diffLfsr = traceSeed;
    set_episode(selectedEpisode);
    logic_init();
    game.episode = selectedEpisode;
    if (selectedStage) logic_set_stage(selectedStage);
    game.stageChanged = 0;
    game.previousInput = JAZZ_INPUT_START;
    stage_art();
    for (f = 0; f < traceFrames; f++) {
        unsigned char rec[DIFF_REC];
        unsigned char nen = 0, nb = 0, i;
        unsigned short es = 0;
        SMS_waitForVBlank();
        render_vblank();
        TS(6); logic_step(diff_next_input()); TE(6);
        for (i = 0; i < JAZZ_MAX_ENEMIES; i++)
            if (game.enemies[i].active) { nen++; es = (unsigned short)(es + game.enemies[i].x * 3 + game.enemies[i].y); }
        for (i = 0; i < JAZZ_MAX_BULLETS; i++) if (game.bullets[i].active) nb++;
        diff_pack(rec, game.frame, game.player.x, game.player.y, game.player.vx, game.player.vy,
                  game.score, game.health, game.lives, game.stage, (unsigned char)game.stageGems, nen, es, nb);
        for (i = 0; i < DIFF_REC; i++) TracePort = rec[i];
        if (game.finished) break;
        if (game.stageChanged) { game.stageChanged = 0; stage_art(); }
        else { TS(7); render_frame(1); TE(7); }
    }
    for (;;) SMS_waitForVBlank();
}
#endif

void main(void)
{
    render_init();
    sound_init();
    fe_options_load();
    SMS_setFrameInterruptHandler(frame_irq);
#ifdef GG_TRACE
    trace_main();
#endif
    for (;;) {
        t_title();
        main_menu();
    }
}
