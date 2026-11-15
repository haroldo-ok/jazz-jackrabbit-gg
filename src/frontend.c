/* Front end for the Game Gear build: the original title, main menu and
 * episode-selection pictures, the planet approach, level/bonus clear and
 * ending screens, the save slots, setup options and the information pages
 * (the Genesis port's lite cartridge front end, src/jj1_scenes.c + main.c).
 *
 * Compiled into code bank 3 and run from the slot-2 window: every ROM data
 * access goes through the fixed-ROM helpers in fe_fixed.c. */

#include <SMSlib.h>
#include "jazz_game.h"
#include "gg_data.h"
#include "fe.h"
#include "music.h"
#include "sound.h"

/* Font sets in VRAM: the plain one at tile 0, an alternative colouring
 * (highlight, yellow) at FONT_B. */
#define FONT_A 0
#define FONT_B 160

static const unsigned int textPal[16] = {
    0x000, 0xFFF, 0x444, 0x0FF, 0x22F, 0x0D0, 0xFA5, 0x888,
    0x07F, 0x0AA, 0x600, 0x000, 0x000, 0x000, 0x000, 0x000
};
#define C_BLACK 0
#define C_WHITE 1
#define C_SHADOW 2
#define C_YELLOW 3
#define C_RED 4
#define C_GREEN 5
#define C_BLUE 6
#define C_GREY 7

u8 feCheat, feCheatStage;
static u8 cheatProgress;

/* ------------------------------------------------------------ text */
static u8 glyph(char c)
{
    u8 i = (u8)((u8)c - 32);
    return (i < 96) ? gg_font_map[i] : 0;
}

/* Font tiles: glyph pixels fg, drop shadow sh, the rest bg. */
static u8 fm0, fm1, fm2, fm3, bm0, bm1, bm2, bm3, sm0, sm1, sm2, sm3;
static u8 fontBuf[32];

static void load_font(u16 tile, u8 fg, u8 bg, u8 sh)
{
    u8 c, y;
    fm0 = (fg & 1) ? 0xFF : 0; fm1 = (fg & 2) ? 0xFF : 0; fm2 = (fg & 4) ? 0xFF : 0; fm3 = (fg & 8) ? 0xFF : 0;
    bm0 = (bg & 1) ? 0xFF : 0; bm1 = (bg & 2) ? 0xFF : 0; bm2 = (bg & 4) ? 0xFF : 0; bm3 = (bg & 8) ? 0xFF : 0;
    sm0 = (sh & 1) ? 0xFF : 0; sm1 = (sh & 2) ? 0xFF : 0; sm2 = (sh & 4) ? 0xFF : 0; sm3 = (sh & 8) ? 0xFF : 0;
    for (c = 0; c < FONT_COUNT; c++) {
        const u8 *src = gg_font + ((u16)c << 3);
        u8 *d = fontBuf, prev = 0;
        for (y = 0; y < 8; y++) {
            u8 v = *src++;
            u8 s = (u8)((prev >> 1) & ~v);
            u8 b = (u8)~(v | s);
            prev = v;
            *d++ = (u8)((v & fm0) | (s & sm0) | (b & bm0));
            *d++ = (u8)((v & fm1) | (s & sm1) | (b & bm1));
            *d++ = (u8)((v & fm2) | (s & sm2) | (b & bm2));
            *d++ = (u8)((v & fm3) | (s & sm3) | (b & bm3));
        }
        SMS_loadTiles(fontBuf, tile + c, 32);
    }
}

static u8 slen(const char *s)
{
    u8 n = 0;
    while (s[n]) n++;
    return n;
}

/* Text in the 20x18 window; x 0xFF centres it.  `font` is FONT_A or FONT_B. */
static void print_at(u8 x, u8 y, const char *s, u16 font)
{
    if (x == 0xFF) x = (u8)((20 - slen(s)) >> 1);
    SMS_setNextTileatXY(6 + x, 3 + y);
    while (*s) SMS_setTile(font + glyph(*s++));
}

static void fill_row(u8 y, u8 x0, u8 x1, u16 tile)
{
    SMS_setNextTileatXY(6 + x0, 3 + y);
    while (x0++ < x1) SMS_setTile(tile);
}

static void put_number(char *p, u8 v)
{
    p[0] = (char)('0' + v / 10);
    p[1] = (char)('0' + v % 10);
}

/* Brightest and darkest entries of a palette (for text over a picture). */
static u8 pal_extreme(const unsigned int *pal, u8 bright)
{
    u8 i, best = 0;
    u16 bestv = bright ? 0 : 0xFFFF;
    for (i = 0; i < 16; i++) {
        u16 c = pal[i];
        u16 v = (u16)((c & 15) * 3 + ((c >> 4) & 15) * 6 + ((c >> 8) & 15));
        if (bright ? (v > bestv) : (v < bestv)) { bestv = v; best = i; }
    }
    return best;
}

static void text_screen_setup(void)
{
    screen_reset();
    GG_loadBGPalette(textPal);
    GG_loadSpritePalette(textPal);
    load_font(FONT_A, C_WHITE, C_BLACK, C_SHADOW);
    load_font(FONT_B, C_YELLOW, C_BLACK, C_SHADOW);
}

/* ------------------------------------------------------------ cheat */
/* UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT 2 1 (the Genesis B A). */
static const u16 cheatCode[10] = {
    PORT_A_KEY_UP, PORT_A_KEY_UP, PORT_A_KEY_DOWN, PORT_A_KEY_DOWN,
    PORT_A_KEY_LEFT, PORT_A_KEY_RIGHT, PORT_A_KEY_LEFT, PORT_A_KEY_RIGHT,
    PORT_A_KEY_2, PORT_A_KEY_1
};

static void draw_cheat(u16 font)
{
    char line[21] = " STAGE SELECT: < 1 >";
    if (!feCheat) return;
    if (feCheatStage >= JAZZ_STAGE_COUNT) {
        line[1] = 'B'; line[2] = 'O'; line[3] = 'N'; line[4] = 'U'; line[5] = 'S';
        line[17] = (char)('1' + feCheatStage - JAZZ_STAGE_COUNT);
    } else line[17] = (char)('1' + feCheatStage);
    print_at(0, 17, line, font);
}

/* Returns 1 when this press completed the code. */
static u8 cheat_feed(u16 pressed)
{
    u16 face = pressed & (PORT_A_KEY_UP | PORT_A_KEY_DOWN | PORT_A_KEY_LEFT | PORT_A_KEY_RIGHT |
                          PORT_A_KEY_1 | PORT_A_KEY_2 | GG_KEY_START);
    if (!face || feCheat) return 0;
    if (face == cheatCode[cheatProgress]) {
        cheatProgress++;
        if (cheatProgress == 10) {
            feCheat = 1;
            sound_play(SFX_YUM, 3);
            return 1;
        }
    } else {
        cheatProgress = (face == cheatCode[0]) ? 1 : 0;
    }
    return 0;
}

static void cheat_select(u16 pressed, u16 up, u16 down, u16 font)
{
    if (!feCheat) return;
    if ((pressed & up) && feCheatStage + 1 < JAZZ_STAGE_COUNT + 3) { feCheatStage++; draw_cheat(font); }
    if ((pressed & down) && feCheatStage) { feCheatStage--; draw_cheat(font); }
}

/* ------------------------------------------------------------ title */
u8 fe_title(void)
{
    u16 pressed;
    u8 white, dark;
    screen_reset();
    GG_loadBGPalette(fe_title_pal);
    GG_loadSpritePalette(fe_title_pal + 16);
    fe_vram_tiles(FE_TITLE_TILES_BANK, FE_TITLE_TILES_ADDR, FE_TITLE_TILES_SIZE, FE_TILE0);
    fe_nametable(FE_TITLE_MAP0_BANK, FE_TITLE_MAP0_ADDR, 0, 1, 20, 13);
    white = pal_extreme(fe_title_pal, 1);
    dark = pal_extreme(fe_title_pal, 0);
    load_font(FONT_A, white, dark, dark);
    fill_row(0, 0, 20, FONT_A);
    fill_row(14, 0, 20, FONT_A);
    fill_row(15, 0, 20, FONT_A);
    fill_row(16, 0, 20, FONT_A);
    fill_row(17, 0, 20, FONT_A);
    print_at(0xFF, 15, "PRESS START", FONT_A);
    draw_cheat(FONT_A);
    SMS_displayOn();
    play_song(SONG_MENUSNG);
    for (;;) {
        pressed = fe_frame();
        if (cheat_feed(pressed)) draw_cheat(FONT_A);
        else cheat_select(pressed, PORT_A_KEY_UP, PORT_A_KEY_DOWN, FONT_A);
        if (pressed & GG_KEY_START) return 0;
    }
}

/* ------------------------------------------------------------ main menu */
static const unsigned char menuMapBank[6] = {
    FE_MENU_MAP0_BANK, FE_MENU_MAP1_BANK, FE_MENU_MAP2_BANK,
    FE_MENU_MAP3_BANK, FE_MENU_MAP4_BANK, FE_MENU_MAP5_BANK
};
static const unsigned int menuMapAddr[6] = {
    FE_MENU_MAP0_ADDR, FE_MENU_MAP1_ADDR, FE_MENU_MAP2_ADDR,
    FE_MENU_MAP3_ADDR, FE_MENU_MAP4_ADDR, FE_MENU_MAP5_ADDR
};

u8 fe_main_menu(u8 option)
{
    u16 pressed;
    u8 white, dark;
    screen_reset();
    GG_loadBGPalette(fe_menu_pal);
    GG_loadSpritePalette(fe_menu_pal + 16);
    fe_vram_tiles(FE_MENU_TILES_BANK, FE_MENU_TILES_ADDR, FE_MENU_TILES_SIZE, FE_TILE0);
    white = pal_extreme(fe_menu_pal, 1);
    dark = pal_extreme(fe_menu_pal, 0);
    load_font(FONT_A, white, dark, dark);
    fill_row(0, 0, 20, FONT_A);
    fill_row(14, 0, 20, FONT_A);
    fill_row(15, 0, 20, FONT_A);
    fill_row(16, 0, 20, FONT_A);
    fill_row(17, 0, 20, FONT_A);
    fe_nametable(menuMapBank[option], menuMapAddr[option], 0, 1, 20, 13);
    print_at(0xFF, 15, "1 SELECT  2 BACK", FONT_A);
    draw_cheat(FONT_A);
    SMS_displayOn();
    play_song(SONG_MENUSNG);
    for (;;) {
        pressed = fe_frame();
        if (cheat_feed(pressed)) draw_cheat(FONT_A);
        cheat_select(pressed, PORT_A_KEY_RIGHT, PORT_A_KEY_LEFT, FONT_A);
        if (pressed & (PORT_A_KEY_UP | PORT_A_KEY_DOWN)) {
            if (pressed & PORT_A_KEY_UP) option = (u8)((option + 5) % 6);
            else option = (u8)((option + 1) % 6);
            fe_nametable(menuMapBank[option], menuMapAddr[option], 0, 1, 20, 13);
        }
        if (pressed & (GG_KEY_START | PORT_A_KEY_1)) return option;
        if (pressed & PORT_A_KEY_2) return 0xFF;
    }
}

/* ------------------------------------------------------------ episodes */
/* The original's list (1-6, A-C, X, Z), the selected row black on white,
 * the episode's picture beside it, its worlds under the heading. */
#define EP_ROW0 7
#define PIC_X 11
#define PIC_W 9
#define PIC_H 7

static void ep_row(u8 row, u8 selected)
{
    char line[21];
    const char *t = fe_ep_title[row];
    u8 i = 0, n = 0;
    u16 font = selected ? FONT_B : FONT_A;
    line[n++] = ' ';
    line[n++] = fe_ep_tag[row][0];
    line[n++] = ' ';
    while (t[i] && n < 20) line[n++] = t[i++];
    while (n < 20) line[n++] = ' ';
    line[20] = 0;
    print_at(0, EP_ROW0 + row, line, font);
}

static void ep_worlds(u8 row)
{
    u8 y, n;
    const char *w;
    for (y = 1; y < 6; y++) fill_row(y, 0, PIC_X, FONT_A);
    if (row >= GG_EPISODES) {
        print_at(0, 3, "3 BONUS", FONT_A);
        print_at(0, 4, "LEVELS", FONT_A);
        return;
    }
    w = gg_episode_info[row].worlds;
    y = 2;
    while (*w && y < 6) {
        char name[12];
        n = 0;
        while (*w && *w != ' ' && n < 11) name[n++] = *w++;
        name[n] = 0;
        while (*w == ' ') w++;
        print_at(0, y++, name, FONT_A);
    }
}

static void ep_picture(u8 row)
{
    const FEPic *p = &fe_eppic[row];
    u8 i;
    /* half the tiles a frame, just after VBlank, then the map and colours */
    u16 n = p->ts >> 5, first = n >> 1;
    SMS_waitForVBlank();
    fe_vram_tiles(p->tb, p->ta, first << 5, FE_TILE0);
    SMS_waitForVBlank();
    fe_vram_tiles(p->tb, p->ta + (first << 5), (n - first) << 5, FE_TILE0 + first);
    SMS_waitForVBlank();
    for (i = 0; i < 16; i++) fe_cram((u8)(16 + i), fe_eppic_pal[row][i]);
    fe_nametable(p->mb, p->ma, PIC_X, 0, PIC_W, PIC_H);
}

u8 fe_episode_menu(u8 row)
{
    u16 pressed;
    u8 r;
    text_screen_setup();
    load_font(FONT_B, C_BLACK, C_WHITE, C_GREY);
    print_at(0, 0, "EPISODES", FONT_A);
    for (r = 0; r < FE_EP_ROWS; r++) ep_row(r, r == row);
    ep_worlds(row);
    SMS_displayOn();
    ep_picture(row);
    play_song(SONG_MENUSNG);
    for (;;) {
        pressed = fe_frame();
        if (pressed & PORT_A_KEY_2) return 0xFF;
        if (pressed & (GG_KEY_START | PORT_A_KEY_1)) return row;
        if (pressed & (PORT_A_KEY_UP | PORT_A_KEY_DOWN)) {
            ep_row(row, 0);
            if (pressed & PORT_A_KEY_UP) row = (u8)((row + FE_EP_ROWS - 1) % FE_EP_ROWS);
            else row = (u8)((row + 1) % FE_EP_ROWS);
            ep_row(row, 1);
            ep_worlds(row);
            ep_picture(row);
        }
    }
}

/* ------------------------------------------------------------ save slots */
u8 fe_slot_menu(u8 saving, u8 message)
{
    u8 valid[FE_SAVE_SLOTS], sel = 0, i, redraw = 1;
    u16 pressed;
    text_screen_setup();
    for (i = 0; i < FE_SAVE_SLOTS; i++) valid[i] = fe_save_valid(i);
    SMS_displayOn();
    for (;;) {
        if (redraw) {
            for (i = 0; i < 18; i++) fill_row(i, 0, 20, FONT_A);
            print_at(0xFF, 2, saving ? "SAVE GAME" : "LOAD GAME", FONT_B);
            for (i = 0; i < FE_SAVE_SLOTS; i++) {
                char line[17] = "  SLOT 1   EMPTY";
                line[0] = (i == sel) ? '>' : ' ';
                line[7] = (char)('1' + i);
                if (valid[i]) { line[11] = 'S'; line[12] = 'A'; line[13] = 'V'; line[14] = 'E'; line[15] = 'D'; }
                print_at(2, (u8)(5 + i * 2), line, (i == sel) ? FONT_B : FONT_A);
            }
            if (message) print_at(0xFF, 13, saving ? "SAVE FAILED" : "SLOT IS EMPTY", FONT_B);
            print_at(0xFF, 15, "1/START SELECT", FONT_A);
            print_at(0xFF, 16, "2 BACK", FONT_A);
            redraw = 0;
        }
        pressed = fe_frame();
        if (pressed & PORT_A_KEY_2) return 0xFF;
        if (pressed & PORT_A_KEY_UP) { sel = (u8)((sel + 3) & 3); redraw = 1; message = 0; }
        if (pressed & PORT_A_KEY_DOWN) { sel = (u8)((sel + 1) & 3); redraw = 1; message = 0; }
        if (pressed & (GG_KEY_START | PORT_A_KEY_1)) {
            if (saving || valid[sel]) return sel;
            message = 1;
            redraw = 1;
        }
    }
}

/* ------------------------------------------------------------ setup */
void fe_setup_menu(void)
{
    u8 sel = 0, redraw = 1, i;
    u16 pressed;
    text_screen_setup();
    SMS_displayOn();
    for (;;) {
        if (redraw) {
            for (i = 0; i < 18; i++) fill_row(i, 0, 20, FONT_A);
            print_at(0xFF, 1, "SETUP OPTIONS", FONT_B);
            print_at(1, 4, sel == 0 ? ">MUSIC" : " MUSIC", sel == 0 ? FONT_B : FONT_A);
            print_at(16, 4, feOptions.music ? "ON " : "OFF", FONT_A);
            print_at(1, 6, sel == 1 ? ">SOUND EFFECTS" : " SOUND EFFECTS", sel == 1 ? FONT_B : FONT_A);
            print_at(16, 6, feOptions.sfx ? "ON " : "OFF", FONT_A);
            print_at(1, 8, sel == 2 ? ">CONTROLS" : " CONTROLS", sel == 2 ? FONT_B : FONT_A);
            print_at(2, 10, feOptions.controls ? "2 FIRE   1 JUMP" : "1 FIRE   2 JUMP", FONT_A);
            print_at(2, 11, "UP+FIRE  WEAPON", FONT_A);
            print_at(0xFF, 14, "UP/DOWN CHOOSE", FONT_A);
            print_at(0xFF, 15, "1/LEFT/RIGHT CHANGE", FONT_A);
            print_at(0xFF, 16, "2/START BACK", FONT_A);
            redraw = 0;
        }
        pressed = fe_frame();
        if (pressed & (PORT_A_KEY_2 | GG_KEY_START)) return;
        if (pressed & PORT_A_KEY_UP) { sel = (u8)((sel + 2) % 3); redraw = 1; }
        if (pressed & PORT_A_KEY_DOWN) { sel = (u8)((sel + 1) % 3); redraw = 1; }
        if (pressed & (PORT_A_KEY_1 | PORT_A_KEY_LEFT | PORT_A_KEY_RIGHT)) {
            if (sel == 0) {
                feOptions.music ^= 1;
                play_song(feOptions.music ? SONG_MENUSNG : 0xFF);
            } else if (sel == 1) {
                feOptions.sfx ^= 1;
                if (!feOptions.sfx) sound_silence();
                else sound_play(SFX_YUM, 1);
            } else {
                feOptions.controls ^= 1;
            }
            fe_options_save();
            redraw = 1;
        }
    }
}

/* ------------------------------------------------------------ info pages */
static const char *const helpPage[] = {
    "INSTRUCTIONS",
    "",
    "LEFT/RIGHT  RUN",
    "1  FIRE   2  JUMP",
    "UP+1  CHANGE WEAPON",
    "DOWN  CROUCH",
    "UP  LOOK UP",
    "START  PAUSE",
    "START THEN 1  SAVE",
    "",
    "FIND THE EXIT SIGN.",
    "BEAT THE GUARDIAN",
    "TO SAVE CARROTUS!",
    "A DIAMOND OPENS",
    "THE BONUS LEVEL.",
    0
};
static const char *const orderPage[] = {
    "ORDER INFO",
    "",
    "THIS CARTRIDGE HOLDS",
    "EVERY EPISODE:",
    "1-6  A-C  AND",
    "HOLIDAY HARE.",
    "",
    "JAZZ JACKRABBIT",
    "BY EPIC MEGAGAMES",
    "(C) 1994",
    "",
    "GAME GEAR PORT OF",
    "THE GENESIS PORT",
    0
};

void fe_info(u8 order)
{
    const char *const *page = order ? orderPage : helpPage;
    u8 y;
    u16 pressed;
    text_screen_setup();
    for (y = 0; page[y]; y++) print_at(0xFF, (u8)(y + 1), page[y], y ? FONT_A : FONT_B);
    SMS_displayOn();
    for (;;) {
        pressed = fe_frame();
        if (pressed & (GG_KEY_START | PORT_A_KEY_1 | PORT_A_KEY_2)) return;
    }
}

/* ------------------------------------------------------------ cutscenes */
/* Waits `frames` frames (0 = until a key), skipped by START, 1 or 2. */
static void wait_keys(u16 frames)
{
    u16 t = 0;
    for (;;) {
        u16 pressed = fe_frame();
        if (pressed & (GG_KEY_START | PORT_A_KEY_1 | PORT_A_KEY_2)) return;
        if (frames && ++t >= frames) return;
    }
}

/* The planet approach: the planet picture and its name, two seconds. */
void fe_planet_screen(u8 episode, u8 planet)
{
    u8 pic = fe_ep_planet[episode][planet];
    u8 i;
    text_screen_setup();
    print_at(0xFF, 10, "NOW APPROACHING", FONT_A);
    if (pic != 0xFF) {
        const FEPic *p = &fe_planet[pic];
        fe_vram_tiles(p->tb, p->ta, p->ts, FE_TILE0);
        for (i = 0; i < 16; i++) fe_cram((u8)(16 + i), fe_planet_pal[pic][i]);
        fe_nametable(p->mb, p->ma, 6, 1, 8, 7);
        print_at(0xFF, 12, fe_planet_name[pic], FONT_B);
    }
    SMS_displayOn();
    play_song(0xFF);
    wait_keys(120);
}

void fe_text(u8 kind, u8 episode)
{
    text_screen_setup();
    switch (kind) {
    case FE_TEXT_LEVEL_CLEAR:
        print_at(0xFF, 8, "LEVEL COMPLETE", FONT_B);
        SMS_displayOn();
        play_song(SONG_ENDLEVEL);
        wait_keys(240);
        break;
    case FE_TEXT_BONUS_CLEAR:
        print_at(0xFF, 8, "BONUS LEVEL", FONT_B);
        print_at(0xFF, 9, "COMPLETE", FONT_B);
        print_at(0xFF, 11, "EXTRA LIFE!", FONT_A);
        SMS_displayOn();
        play_song(SONG_ENDLEVEL);
        wait_keys(240);
        break;
    case FE_TEXT_ENDING:
        print_at(0xFF, 5, "CONGRATULATIONS!", FONT_B);
        print_at(0xFF, 8, gg_episode_info[episode].title, FONT_A);
        print_at(0xFF, 10, "IS COMPLETE", FONT_A);
        print_at(0xFF, 14, "PRESS START", FONT_A);
        SMS_displayOn();
        play_song(SONG_BONUS);
        wait_keys(0);
        break;
    default:
        print_at(0xFF, 6, "GAME OVER", FONT_B);
        print_at(0xFF, 9, "TRY AGAIN, JAZZ!", FONT_A);
        print_at(0xFF, 13, "PRESS START", FONT_A);
        SMS_displayOn();
        play_song(0xFF);
        wait_keys(0);
        break;
    }
    play_song(0xFF);
}
