#ifndef JAZZ_GAME_H
#define JAZZ_GAME_H

/* Game Gear build of the Jazz Jackrabbit simulation.
 *
 * Same game model as the Genesis port (original JJ1 maps, masks, event grids
 * and the 8.8 fixed-point movement model), with two Z80-driven changes:
 *  - the whole state is ONE global (`game`), so SDCC addresses every field
 *    directly instead of through a pointer (much smaller, faster code);
 *  - the 2 KB "destroyed" bitmap is replaced by an append-only list, which
 *    keeps the state inside the Game Gear's 8 KB of RAM.
 * World coordinates stay in ORIGINAL pixels (32 px blocks); only the renderer
 * halves them, which is what halves the background tiles and sprites. */

#ifdef GG_HOST
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
#else
typedef unsigned char u8;
typedef unsigned int u16;
typedef unsigned long u32;
typedef signed char s8;
typedef signed int s16;
typedef signed long s32;
#endif

#define JAZZ_STAGE_COUNT 7

#define JAZZ_MAX_ENEMIES 6
#define JJ1_BOSS_SLOT_SPAN 3
#define JAZZ_MAX_DESTRUCT_HITS 6
#define JAZZ_MAX_DESTROYED 64
#define JAZZ_ENEMY_W 14
#define JAZZ_ENEMY_H 16
#define JAZZ_MAX_BULLETS 6
#define PLAYER_W 14
#define PLAYER_H 22

#define JAZZ_INVINCIBLE_FRAMES 600
#define JAZZ_AMMO_PER_CRATE 15

#define JAZZ_SHOT_BLASTER 0
#define JAZZ_SHOT_TOASTER 1
#define JAZZ_SHOT_UPWARD  2
#define JAZZ_SHOT_BOUNCER 3
#define JAZZ_SHOT_SPECIAL 4
#define JAZZ_SHOT_TYPES   5
#define JAZZ_FASTFEET_FRAMES   600
#define JAZZ_PXS_FASTRUN    1800
#define JAZZ_HIGHJUMP_BONUS 16

#define JJ1_PLAYER_STAND  0
#define JJ1_PLAYER_WALK   1
#define JJ1_PLAYER_RUN    2
#define JJ1_PLAYER_JUMP   3
#define JJ1_PLAYER_FALL   4
#define JJ1_PLAYER_SHOOT  5
#define JJ1_PLAYER_CROUCH 6
#define JJ1_PLAYER_LOOKUP 7
#define JJ1_PLAYER_SKID   8
#define JJ1_PLAYER_HURT   9
#define JJ1_PLAYER_SPRING 10
#define JJ1_PLAYER_BOARD  11
#define JJ1_PLAYER_STATES 12

#define JAZZ_INPUT_LEFT  0x0001
#define JAZZ_INPUT_RIGHT 0x0002
#define JAZZ_INPUT_JUMP  0x0004
#define JAZZ_INPUT_FIRE  0x0008
#define JAZZ_INPUT_START 0x0010
#define JAZZ_INPUT_SWITCH 0x0020
#define JAZZ_INPUT_DOWN   0x0040
#define JAZZ_INPUT_UP     0x0080

#define JAZZ_PXS_WALK       640
#define JAZZ_PXS_RUN        1300
#define JAZZ_PYS_FALL       1400
#define JAZZ_PYS_JUMP       (-1400)
#define JAZZ_PXA_REVERSE    112
#define JAZZ_PXA_STOP       64
#define JAZZ_PXA_WALK       64
#define JAZZ_PXA_RUN        64
#define JAZZ_PYA_GRAVITY    144
#define JAZZ_PYO_JUMP       84
#define JAZZ_NO_JUMP_TARGET 0x7FFF

enum {
    JAZZ_EVENT_NONE  = 0,
    JAZZ_EVENT_JUMP  = 1,
    JAZZ_EVENT_FIRE  = 2,
    JAZZ_EVENT_GEM   = 4,
    JAZZ_EVENT_HURT  = 8,
    JAZZ_EVENT_EXIT  = 16,
    JAZZ_EVENT_KILL  = 32,
    JAZZ_EVENT_HEALTH = 64,
    JAZZ_EVENT_LIFE   = 128
};

/* Behaviour classes (jj1_events.h in the Genesis port). */
enum {
    JJ1_CLASS_NONE = 0, JJ1_CLASS_ITEM, JJ1_CLASS_ENEMY_WALK, JJ1_CLASS_ENEMY_FLY,
    JJ1_CLASS_HAZARD, JJ1_CLASS_SPRING, JJ1_CLASS_ONEWAY, JJ1_CLASS_END,
    JJ1_CLASS_DESTRUCT, JJ1_CLASS_TUBE, JJ1_CLASS_BRIDGE, JJ1_CLASS_UNBOARD,
    JJ1_CLASS_BOSS
};
enum {
    JJ1_ITEM_SCORE = 0, JJ1_ITEM_HEALTH, JJ1_ITEM_LIFE, JJ1_ITEM_FASTFEET,
    JJ1_ITEM_AMMO, JJ1_ITEM_INVINCIBLE, JJ1_ITEM_SHIELD, JJ1_ITEM_HIGHJUMP,
    JJ1_ITEM_BIRD, JJ1_ITEM_AIRBOARD
};

typedef struct {
    u8 klass;
    u8 param;
    u8 points;
    u8 strength;
} Jj1EventInfo;

typedef struct {
    s16 x, y;
    s16 subX, subY;
    s16 vx, vy;
    s16 jumpTargetY;
    u8 facing;
    u8 onGround;
    u8 springJump;
    u8 shotType;
    u8 weaponsOwned;
    u8 ammo[JAZZ_SHOT_TYPES];
    u16 invincibleTime;
    u8 flying;
    u8 bird;
    s16 birdX, birdY;
    u8 birdCooldown;
    u16 fastFeetTime;
    u8 shield;
    u8 highJump;
    u8 inTube;
} JazzPlayer;

typedef struct {
    s16 x, y;
    s16 homeX, homeY;
    s8 direction;
    u8 active;
    u8 klass;
    u8 gridX, gridY;
    u8 hitPoints;
    u8 phase;
    u8 bossFacing;
} JazzEnemy;

typedef struct {
    s16 x, y;
    s16 vx, vy;
    s16 subX, subY;
    s8 gravity;
    u8 behaviour;
    u8 hostile;
    u8 active;
} JazzBullet;

typedef struct {
    u8 gridX, gridY;
    u8 hits;
    u8 active;
} JazzDestructHit;

typedef struct {
    u8 destroyedCount;   /* MUST stay first: gg_world.c's asm reads it at _game */
    JazzPlayer player;
    JazzEnemy enemies[JAZZ_MAX_ENEMIES];
    JazzBullet bullets[JAZZ_MAX_BULLETS];
    /* One bit per 32x32 event cell: item collected / enemy killed. */
    u8 taken[(256 * 64) / 8];
    /* Destroyed blocks, in the order they fell (the renderer replays new
     * entries).  destroyedCols marks columns holding one, as a quick reject. */
    u8 destroyedX[JAZZ_MAX_DESTROYED];
    u8 destroyedY[JAZZ_MAX_DESTROYED];
    u8 destroyedCols[32];
    JazzDestructHit destructHits[JAZZ_MAX_DESTRUCT_HITS];
    u16 enemyScanColumn;
    u8 bossGridX, bossGridY;
    u8 bossPresent;
    u16 previousInput;
    u16 frame;
    u16 score;
    u8 stage;
    u8 lives;
    u8 health;
    u16 stageGems;
    u16 stageGemTotal;
    u8 fireCooldown;
    u8 invulnerability;
    u8 paused;
    u8 transitionTimer;
    u8 finished;
    u8 events;
    u8 animState;     /* JJ1_PLAYER_*, derived from physics each step */
    u8 stageChanged;  /* set when build_stage() ran: renderer reloads art */
} JazzGame;

extern JazzGame game;

/* ---- banked game logic (gg_logic.c, mapped through the slot-2 window) ---- */
void jazz_game_init(void);
void jazz_step(u16 input);
void jazz_set_stage(u8 stage);

/* ---- fixed-ROM world accessors (gg_world.c) ---- */
extern Jj1EventInfo gg_einfo[128];
void gg_world_load(u8 stage);
u8 gg_event(s16 bx, s16 by);
u8 gg_solid(s16 x, s16 y);        /* sides/ceilings: one-way and bridges pass */
u8 gg_down_solid(s16 x, s16 y);   /* feet: one-way platforms and bridge decks land */
u8 gg_down_solid_raw(s16 x, s16 y); /* same, ignoring destroyed blocks */
u8 gg_cell_destroyed(u8 gx, u8 gy);
u8 gg_side3(s16 x, s16 y);       /* (x, y+4) | (x, y+11) | (x, y+19) solid */
u8 gg_feet3(s16 x, s16 y);       /* (x+2|x+7|x+11, y) feet-solid */
u8 gg_head3(s16 x, s16 y);       /* (x+2|x+7|x+11, y) solid */
u8 gg_ground_dist(s16 x, s16 y); /* 0..8 rows down to feet-solid, else 9 */
void gg_event_column(u8 gx, u8 gy0, u8 n, u8 *out);
extern u8 scanGx, scanGy0, scanN, scanIds[16];
void gg_scan_column(void);   /* scanIds[0..scanN) = events down column scanGx from row scanGy0 */
#define gg_event_info(id) (&gg_einfo[(id)])

u8 jazz_event_taken(u8 gridX, u8 gridY);

#endif
