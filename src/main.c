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

#define LOGIC_BANK 2

SMS_EMBED_SEGA_ROM_HEADER(9999, 0);
SMS_EMBED_SDSC_HEADER_AUTO_DATE(0, 1, "haroldo-ok / Claude", "Jazz Jackrabbit GG",
                                "Game Gear port of the Genesis JJ1 homebrew port");

JazzGame game;

enum { MODE_TITLE, MODE_PLAY, MODE_END };
u8 mode;
u8 selectedStage;
u16 keys, prevKeys;
u16 playInput;
volatile u8 vblankTicks;   /* bumped by the frame interrupt */
u8 lastTick;
u8 stepsRun;               /* game frames run by the last loop pass */
u8 benchMode;
u8 stepHist[8];            /* passes by video frames elapsed (profiling) */
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

static void frame_irq(void)
{
    vblankTicks++;
    if (skyOn) {
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

/* ---- trampolines into the banked game logic ---- */
static void logic_init(void) { SMS_mapROMBank(LOGIC_BANK); jazz_game_init(); }
static void logic_step(u16 in) { SMS_mapROMBank(LOGIC_BANK); jazz_step(in); }
static void logic_set_stage(u8 s) { SMS_mapROMBank(LOGIC_BANK); jazz_set_stage(s); }

/* ---- BG text (title / end screens) ---- */
static u8 bg_glyph(char c)
{
    if (c >= '0' && c <= '9') return (u8)(1 + (c - '0'));
    if (c >= 'A' && c <= 'Z') return (u8)(11 + (c - 'A'));
    switch (c) {
    case '-': return 37; case '!': return 38; case ':': return 39; case '/': return 40;
    case '.': return 41; case ',': return 42; case '<': return 43; case '>': return 44;
    case '\'': return 45; case '*': return 46; case '#': return 47;
    }
    return 0;
}

/* Text inside the 20x18 visible GG window, centred when x == 0xFF. */
static void print_at(u8 x, u8 y, const char *s)
{
    u8 len = 0;
    while (s[len]) len++;
    if (x == 0xFF) x = (u8)((20 - len) >> 1);
    SMS_setNextTileatXY(6 + x, 3 + y);
    while (*s) SMS_setTile(bg_glyph(*s++));
}

static const unsigned int titlePal[16] = {
    0x000, 0xFFF, 0x444, 0x0AF, 0x08F, 0x2F2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void clear_screen(void)
{
    sky_enable(0);
    SMS_displayOff();
    SMS_VRAMmemsetW(0x3800, 0, 32 * 28 * 2);
    SMS_initSprites();
    SMS_finalizeSprites();
    SMS_copySpritestoSAT();
    SMS_setBGScrollX(0);
    SMS_setBGScrollY(0);
    GG_loadBGPalette(titlePal);
    GG_loadSpritePalette(titlePal);
    render_load_font_bg();
}

static void draw_level_select(void)
{
    char line[12] = "LEVEL < 1 >";
    line[8] = (char)('1' + selectedStage);
    print_at(0xFF, 11, line);
}

static void show_title(void)
{
    clear_screen();
    print_at(0xFF, 2, "JAZZ JACKRABBIT");
    print_at(0xFF, 4, "GAME GEAR PORT");
    print_at(0xFF, 7, "EPISODE 1");
    print_at(0xFF, 8, "DIAMONDUS");
    print_at(0xFF, 9, "TUBELECTRIC MEDIVO");
    draw_level_select();
    print_at(0xFF, 13, "PRESS START");
    print_at(0xFF, 15, "1 FIRE   2 JUMP");
    print_at(0xFF, 16, "UP+1 WEAPON");
    SMS_displayOn();
    music_play(SONG_TITLE);
    mode = MODE_TITLE;
}

static void show_end(const char *title, const char *message, u8 song)
{
    sound_silence();
    music_play(song);
    clear_screen();
    print_at(0xFF, 6, title);
    print_at(0xFF, 9, message);
    print_at(0xFF, 13, "PRESS START");
    SMS_displayOn();
    mode = MODE_END;
}

static void stage_art(void)
{
    music_play(gg_stage_song[game.stage]);
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

static void begin_play(void)
{
    sound_silence();
    logic_init();
    if (selectedStage) logic_set_stage(selectedStage);
    game.stageChanged = 0;
    game.previousInput = JAZZ_INPUT_START;
    stage_art();
    mode = MODE_PLAY;
}

static u16 input_from_keys(u16 k)
{
    u16 in = 0;
    if (k & PORT_A_KEY_LEFT) in |= JAZZ_INPUT_LEFT;
    if (k & PORT_A_KEY_RIGHT) in |= JAZZ_INPUT_RIGHT;
    if (k & PORT_A_KEY_UP) in |= JAZZ_INPUT_UP;
    if (k & PORT_A_KEY_DOWN) in |= JAZZ_INPUT_DOWN;
    if (k & PORT_A_KEY_2) in |= JAZZ_INPUT_JUMP;
    if (k & PORT_A_KEY_1) {
        /* Up + 1 cycles weapons (the Genesis C button), except on the
           airboard, where up is thrust. */
        if ((k & PORT_A_KEY_UP) && !game.player.flying) in |= JAZZ_INPUT_SWITCH;
        else in |= JAZZ_INPUT_FIRE;
    }
    if (k & GG_KEY_START) in |= JAZZ_INPUT_START;
    return in;
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
    if (traceSeed) diffLfsr = traceSeed;
    begin_play();
    for (f = 0; f < traceFrames; f++) {
        unsigned char rec[DIFF_REC];
        unsigned char nen = 0, nb = 0, i;
        unsigned short es = 0;
        SMS_waitForVBlank();
        render_vblank();
        logic_step(diff_next_input());
        for (i = 0; i < JAZZ_MAX_ENEMIES; i++)
            if (game.enemies[i].active) { nen++; es = (unsigned short)(es + game.enemies[i].x * 3 + game.enemies[i].y); }
        for (i = 0; i < JAZZ_MAX_BULLETS; i++) if (game.bullets[i].active) nb++;
        diff_pack(rec, game.frame, game.player.x, game.player.y, game.player.vx, game.player.vy,
                  game.score, game.health, game.lives, game.stage, (unsigned char)game.stageGems, nen, es, nb);
        for (i = 0; i < DIFF_REC; i++) TracePort = rec[i];
        if (game.finished) break;
        if (game.stageChanged) { game.stageChanged = 0; stage_art(); }
        else render_frame(1);
    }
    for (;;) SMS_waitForVBlank();
}
#endif

void main(void)
{
    render_init();
    sound_init();
    SMS_setFrameInterruptHandler(frame_irq);
#ifdef GG_TRACE
    trace_main();
#endif
    show_title();
    for (;;) {
        SMS_waitForVBlank();
        if (mode == MODE_PLAY) render_vblank();
        prevKeys = keys;
        keys = SMS_getKeysStatus();
        if (mode == MODE_TITLE) {
            u16 pressed = keys & ~prevKeys;
            if ((pressed & PORT_A_KEY_RIGHT) && (selectedStage + 1 < JAZZ_STAGE_COUNT)) {
                selectedStage++;
                draw_level_select();
            } else if ((pressed & PORT_A_KEY_LEFT) && selectedStage) {
                selectedStage--;
                draw_level_select();
            }
            if (pressed & GG_KEY_START) begin_play();
        } else if (mode == MODE_PLAY) {
            /* Fixed timestep: one game frame per elapsed video frame, so the
               game keeps its real speed even when a frame of rendering does
               not fit in one refresh (the picture then updates at 30 Hz). */
            u8 n = (u8)(vblankTicks - lastTick);
            u8 ev = 0, k;
            lastTick += n;
            stepHist[(n > 7) ? 7 : n]++;
#ifdef PASS_TRACE
            stepHistLast = n;
#endif
            if (!n || benchMode) n = 1;
            if (n > 6) n = 6;
            playInput = input_from_keys(keys);
            stepsRun = 0;
            for (k = 0; k < n; k++) {
                logic_step(playInput);
                stepsRun++;
                ev |= game.events;
                if (game.stageChanged || game.finished) break;
            }
            sound_events(ev);
            music_pause(game.paused);
            if (game.stageChanged) {
                game.stageChanged = 0;
                stage_art();
            } else {
                render_frame(stepsRun);
            }
#ifdef PASS_TRACE
            {
                extern u8 cacheMisses, spriteUploads, winMoved;
                PassPort = stepHistLast; PassPort = stepsRun; PassPort = cacheMisses;
                PassPort = spriteUploads; PassPort = winMoved;
            }
#endif
            if (game.finished) {
                if (game.lives) show_end("EPISODE COMPLETE", "CARROTUS IS SAFE", SONG_ENDLEVEL);
                else show_end("GAME OVER", "TRY AGAIN, JAZZ!", 0xFF);
            }
        } else {
            if ((keys & ~prevKeys) & GG_KEY_START) show_title();
        }
    }
}
