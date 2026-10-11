/* Jazz Jackrabbit game logic for the Game Gear.
 *
 * Ported from the Genesis port's game_core.c + jj1_runtime.c: the same JJ1
 * movement model (8.8 fixed point per frame, two-tier walk/run acceleration,
 * target-height variable jump, gravity only while rising, mask ramps), event
 * classes, items, enemies, springs, tubes, bridges, destructibles and the
 * guardian.  Coordinates are original JJ1 pixels; the renderer halves them.
 *
 * This file is compiled into a switchable ROM bank and runs from the slot-2
 * window.  Everything it calls outside itself (gg_solid, gg_event, ...) lives
 * in fixed ROM and restores slot 2 before returning. */

#include "jazz_game.h"
#include <stddef.h>

#define ENEMY_W JAZZ_ENEMY_W
#define ENEMY_H JAZZ_ENEMY_H
#define WORLD_W (256 * 32)
#define WORLD_H (64 * 32)

#define g (&game)

void gg_stage_facts(u8 stage, u8 *sx, u8 *sy, u16 *items, u8 *bossPresent, u8 *bx, u8 *by);

/* ------------------------------------------------------------ collision */

#define pt_solid(x, y) gg_solid((x), (y))
#define down_pt_solid(x, y) gg_down_solid((x), (y))

static u8 rect_hits_solid(s16 x, s16 y, s16 w, s16 h)
{
    s16 px, py, x1, y1;
    if ((x < 0) || (y < 0) || ((x + w - 1) >= WORLD_W) || ((y + h - 1) >= WORLD_H)) return 1;
    x1 = x + w - 1;
    y1 = y + h - 1;
    for (py = y; py < y + h; py += 4)
        for (px = x; px < x + w; px += 4)
            if (pt_solid(px, py)) return 1;
    for (py = y; py < y + h; py += 4)
        if (pt_solid(x1, py)) return 1;
    for (px = x; px < x + w; px += 4)
        if (pt_solid(px, y1)) return 1;
    return pt_solid(x1, y1);
}

static u8 rect_overlap(s16 ax, s16 ay, s16 aw, s16 ah, s16 bx, s16 by, s16 bw, s16 bh)
{
    return (ax < (bx + bw)) && ((ax + aw) > bx) && (ay < (by + bh)) && ((ay + ah) > by);
}


static void mark_event_taken(u8 gridX, u8 gridY)
{
    u16 bit = ((u16)gridY << 8) + gridX;
    g->taken[bit >> 3] |= (u8)(1 << (bit & 7));
}

/* --------------------------------------------------------- runtime queries */

#define cell_info(bx, by) gg_event_info(gg_event((bx), (by)))

/* The (at most 2x2) event cells under Jazz's 14x22 body, row-major, as the
 * Genesis spring/tube/end/item loops visit them.  Gathered once per phase
 * instead of once per query. */
static u8 pcN;
static u8 pcX[4], pcY[4], pcId[4];
static s16 pcAtX, pcAtY;

static void gather_player_cells(void)
{
    s16 x = g->player.x, y = g->player.y;
    u8 bx, by, bx0, bx1, by0, by1;
    /* The event grid is static, so the cells only change when Jazz moves:
       the pre-move gather nearly always finds last frame's final position. */
    if ((x == pcAtX) && (y == pcAtY)) return;
    pcN = 0;
    pcAtX = x;
    pcAtY = y;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    bx0 = (u8)((u16)x >> 5);
    bx1 = (u8)((u16)(x + PLAYER_W - 1) >> 5);
    by0 = (u8)((u16)y >> 5);
    by1 = (u8)((u16)(y + PLAYER_H - 1) >> 5);
    if (by1 > 63) by1 = 63;
    for (by = by0; by <= by1; by++)
        for (bx = bx0; ; bx++) {
            pcX[pcN] = bx;
            pcY[pcN] = by;
            pcId[pcN] = gg_event(bx, by);
            pcN++;
            if (bx == bx1) break;
        }
}

/* Upward springs: retarget the jump ascent so the FEET rise to
 * magnitude * 21 px above the spring block. */
static u16 spring_height(s16 *springTopY)
{
    u8 i;
    for (i = 0; i < pcN; i++) {
        const Jj1EventInfo *info = gg_event_info(pcId[i]);
        if (info->klass == JJ1_CLASS_SPRING) {
            *springTopY = (s16)pcY[i] << 5;
            return (u16)info->param * 21;
        }
    }
    return 0;
}

/* Sucker tube: 0 none, 1 horizontal push, 2 vertical launch. */
static u8 tube_push(s16 *pushX, s16 *targetY)
{
    u8 i;
    *pushX = 0;
    *targetY = 0;
    for (i = 0; i < pcN; i++) {
        const Jj1EventInfo *info = gg_event_info(pcId[i]);
        if (info->klass == JJ1_CLASS_TUBE) {
            s16 mag = (s16)(s8)info->param;
            *pushX = (s16)(mag * 171);
            if (info->strength) {
                *targetY = (s16)(((s16)pcY[i] << 5) - ((s16)info->strength * 3));
                return 2;
            }
            return 1;
        }
    }
    return 0;
}

static u8 touches_end(void)
{
    u8 i;
    for (i = 0; i < pcN; i++)
        if (gg_event_info(pcId[i])->klass == JJ1_CLASS_END) return 1;
    return 0;
}

static u8 startGridX, startGridY;
static u8 scanOffset, scanSpan;   /* enemy activation round-robin */

static void place_player(void)
{
    s16 spawnX = ((s16)startGridX << 5) + 8;
    s16 spawnY = ((s16)startGridY << 5) - 22;
    if (spawnY < 0) spawnY = 0;
    while ((spawnY < ((64 * 32) - 23)) &&
           !down_pt_solid(spawnX + 2, spawnY + 22) &&
           !down_pt_solid(spawnX + 7, spawnY + 22) &&
           !down_pt_solid(spawnX + 11, spawnY + 22)) spawnY++;
    g->player.x = spawnX;
    g->player.y = spawnY;
    g->player.subX = 0;
    g->player.subY = 0;
    g->player.vx = 0;
    g->player.vy = 0;
    g->player.jumpTargetY = JAZZ_NO_JUMP_TARGET;
    g->player.onGround = 1;
}

/* ------------------------------------------------------------- stage setup */

static void clear_game(void)
{
    u16 i;
    u8 *p;
    for (i = 0; i < JAZZ_MAX_ENEMIES; i++) g->enemies[i].active = 0;
    for (i = 0; i < JAZZ_MAX_BULLETS; i++) g->bullets[i].active = 0;
    p = g->taken;
    for (i = 0; i < sizeof(g->taken); i++) *p++ = 0;
    for (i = 0; i < 32; i++) g->destroyedCols[i] = 0;
    g->destroyedCount = 0;
    for (i = 0; i < JAZZ_MAX_DESTRUCT_HITS; i++) g->destructHits[i].active = 0;
    g->enemyScanColumn = 0;
    scanSpan = 0;
    pcAtX = 0x7FFF;   /* new event grid: regather */
}

static void build_stage(u8 stage)
{
    /* Progress belongs to the player: lives, score, weapons and the permanent
       powerups carry over between levels. */
    u8 carryShot = g->player.shotType;
    u8 carryOwned = g->player.weaponsOwned;

    clear_game();
    gg_world_load(stage);
    g->stage = stage;
    g->stageChanged = 1;
    g->health = 5;
    g->fireCooldown = 0;
    g->invulnerability = 0;
    g->paused = 0;
    g->transitionTimer = 0;
    g->stageGems = 0;
    g->player.subX = 0;
    g->player.subY = 0;
    g->player.vx = 0;
    g->player.vy = 0;
    g->player.jumpTargetY = JAZZ_NO_JUMP_TARGET;
    g->player.springJump = 0;
    g->player.shotType = carryShot;
    g->player.weaponsOwned = (u8)(carryOwned | (1 << JAZZ_SHOT_BLASTER));
    g->player.invincibleTime = 0;
    g->player.fastFeetTime = 0;
    g->player.flying = 0;
    g->player.inTube = 0;
    g->player.facing = 1;
    g->player.onGround = 1;

    /* Precomputed by the converter: start cell, item count and the guardian
       cell, so the Z80 never scans all 16384 event cells. */
    gg_stage_facts(stage, &startGridX, &startGridY, &g->stageGemTotal,
                   &g->bossPresent, &g->bossGridX, &g->bossGridY);
    place_player();
}

void jazz_game_init(void)
{
    u16 i;
    u8 *p = (u8 *)&game;
    for (i = 0; i < sizeof(JazzGame); i++) *p++ = 0;
    g->lives = 3;
    build_stage(0);
}

void jazz_set_stage(u8 stage)
{
    build_stage(stage);
}

/* ----------------------------------------------------------------- player */

static void respawn_or_lose_life(void)
{
    if (g->lives) g->lives--;
    if (!g->lives) {
        g->finished = 1;
        return;
    }
    g->health = 5;
    g->player.vx = 0;
    g->player.vy = 0;
    g->player.jumpTargetY = JAZZ_NO_JUMP_TARGET;
    g->player.springJump = 0;
    g->player.onGround = 1;
    place_player();
    g->invulnerability = 90;
}

void hurt_player(void)
{
    if (g->invulnerability) return;
    if (g->player.invincibleTime) return;
    if (g->player.shield) {
        g->player.shield--;
        g->invulnerability = 55;
        g->events |= JAZZ_EVENT_HURT;
        return;
    }
    g->events |= JAZZ_EVENT_HURT;
    g->invulnerability = 55;
    if (g->health) g->health--;
    if (!g->health) respawn_or_lose_life();
}

/* Side probe at the leading edge: y+4, y+11 (PLAYER_H/2), y+19 (PLAYER_H-3). */
#define player_side_blocked(x, y, step) \
    gg_side3((s16)((x) + (((step) > 0) ? (PLAYER_W - 1) : 0)), (y))

/* Feet (one-way aware) or head probe at x+2, x+7 (PLAYER_W/2), x+11. */
#define player_vertical_blocked(x, y, step) \
    (((step) > 0) ? gg_feet3((x), (s16)((y) + (PLAYER_H - 1))) : gg_head3((x), (y)))

/* Distance from the feet to the next solid 4px mask cell (9 = none). */
#define ground_distance(x, y) \
    ((s8)gg_ground_dist((s16)((x) + (PLAYER_W >> 1)), (s16)((y) + PLAYER_H)))

/* Jazz is blocked one pixel ahead (x + step): try to follow a descending
 * ramp or climb a rising one by at most a 4 px mask cell, exactly as the
 * Genesis loop does.  Returns 1 when the step was taken, else stops Jazz. */
u8 blocked_step(s8 step)
{
    if (g->player.onGround) {
        s8 currentGround = ground_distance(g->player.x, g->player.y);
        s8 forwardGround = ground_distance(g->player.x + step, g->player.y);
        /* Descending ramp: follow its mask down. */
        if ((forwardGround > currentGround) && (forwardGround <= 8)) {
            s8 drop = forwardGround - currentGround;
            if (!player_side_blocked(g->player.x + step, g->player.y + drop, step)) {
                g->player.x += step;
                g->player.y += drop;
                return 1;
            }
        }
        /* Rising ramp: lift at most one 4px mask cell. */
        if (forwardGround <= currentGround) {
            s8 lift;
            for (lift = 1; lift <= 4; lift++) {
                if (!player_side_blocked(g->player.x + step, g->player.y - lift, step) &&
                    !player_vertical_blocked(g->player.x + step, g->player.y - lift, -1)) {
                    g->player.y -= lift;
                    g->player.x += step;
                    return 1;
                }
            }
        }
    }
    g->player.vx = 0;
    return 0;
}

/* Field offsets the asm below relies on. */
#define OFF_PX 1
#define OFF_PY 3
#define OFF_PVY 11
#define OFF_PJUMPT 13
#define OFF_PGROUND 16
#ifndef GG_HOST
typedef char chk_px[(offsetof(JazzGame, player.x) == OFF_PX) ? 1 : -1];
typedef char chk_py[(offsetof(JazzGame, player.y) == OFF_PY) ? 1 : -1];
typedef char chk_pvy[(offsetof(JazzGame, player.vy) == OFF_PVY) ? 1 : -1];
typedef char chk_pjt[(offsetof(JazzGame, player.jumpTargetY) == OFF_PJUMPT) ? 1 : -1];
typedef char chk_pg[(offsetof(JazzGame, player.onGround) == OFF_PGROUND) ? 1 : -1];
#endif

#ifdef GG_HOST
static void move_player_x(s8 amount)
{
    s8 step = (amount < 0) ? -1 : 1;
    s16 openCol = 0x7FFF, openY = 0x7FFF;
    while (amount) {
        /* The side probe only changes with its 4 px mask column (and y): an
           open column at the same height stays open. */
        s16 probeX = g->player.x + step + ((step > 0) ? (PLAYER_W - 1) : 0);
        s16 col = probeX >> 2;
        if ((col == openCol) && (g->player.y == openY)) {
            g->player.x += step;
            amount -= step;
            continue;
        }
        if (gg_side3(probeX, g->player.y)) {
            if (!blocked_step(step)) return;
            amount -= step;
            continue;
        }
        openCol = col;
        openY = g->player.y;
        g->player.x += step;
        amount -= step;
    }
}

static void move_player_y(s8 amount)
{
    s8 step = (amount < 0) ? -1 : 1;
    s16 lastRow = 0x7FFF;
    g->player.onGround = 0;
    while (amount) {
        /* x is fixed in this loop and a probe only changes with the 4 px
           mask row, so a row already found open stays open. */
        s16 probeY = g->player.y + step + ((step > 0) ? (PLAYER_H - 1) : 0);
        s16 row = probeY >> 2;
        if ((row != lastRow) &&
            ((step > 0) ? gg_feet3(g->player.x, probeY) : gg_head3(g->player.x, probeY))) {
            if (step > 0) g->player.onGround = 1;
            else g->player.jumpTargetY = JAZZ_NO_JUMP_TARGET;
            g->player.vy = 0;
            return;
        }
        lastRow = row;
        g->player.y += step;
        amount -= step;
    }
}
#else
s8 mxAmount, mxStep;
s16 mxOpenCol, mxOpenY, mxProbe, mxCol;

/* Z80 versions of the two loops above.  A = amount. */
static void move_player_x(s8 amount) __naked
{
    (void)amount;
    __asm
    or  a, a
    ret z
    ld  (_mxAmount), a
    ld  b, #1
    bit 7, a
    jr  z, 09801$
    ld  b, #0xFF
09801$:
    ld  a, b
    ld  (_mxStep), a
    ld  hl, #0x7FFF
    ld  (_mxOpenCol), hl
    ld  (_mxOpenY), hl
09802$:
    ld  a, (_mxAmount)
    or  a, a
    ret z
    ld  hl, (_game + OFF_PX)
    ld  a, (_mxStep)
    ld  e, a
    rlca
    sbc a, a
    ld  d, a
    add hl, de
    bit 7, e
    jr  nz, 09803$
    ld  de, #13
    add hl, de
09803$:
    ld  (_mxProbe), hl
    sra h
    rr  l
    sra h
    rr  l
    ld  (_mxCol), hl
    ld  de, (_mxOpenCol)
    or  a, a
    sbc hl, de
    jr  nz, 09804$
    ld  hl, (_game + OFF_PY)
    ld  de, (_mxOpenY)
    or  a, a
    sbc hl, de
    jr  z, 09806$
09804$:
    ld  hl, (_mxProbe)
    ld  de, (_game + OFF_PY)
    call _gg_side3
    or  a, a
    jr  nz, 09807$
    ld  hl, (_mxCol)
    ld  (_mxOpenCol), hl
    ld  hl, (_game + OFF_PY)
    ld  (_mxOpenY), hl
09806$:
    ld  a, (_mxStep)
    ld  e, a
    rlca
    sbc a, a
    ld  d, a
    ld  hl, (_game + OFF_PX)
    add hl, de
    ld  (_game + OFF_PX), hl
09808$:
    ld  a, (_mxStep)
    ld  b, a
    ld  a, (_mxAmount)
    sub a, b
    ld  (_mxAmount), a
    jr  09802$
09807$:
    ld  a, (_mxStep)
    call _blocked_step
    or  a, a
    ret z
    jr  09808$
    __endasm;
}

static void move_player_y(s8 amount) __naked
{
    (void)amount;
    __asm
    ld  b, a
    xor a, a
    ld  (_game + OFF_PGROUND), a
    ld  a, b
    or  a, a
    ret z
    ld  (_mxAmount), a
    ld  b, #1
    bit 7, a
    jr  z, 09821$
    ld  b, #0xFF
09821$:
    ld  a, b
    ld  (_mxStep), a
    ld  hl, #0x7FFF
    ld  (_mxOpenCol), hl
09822$:
    ld  a, (_mxAmount)
    or  a, a
    ret z
    ld  hl, (_game + OFF_PY)
    ld  a, (_mxStep)
    ld  e, a
    rlca
    sbc a, a
    ld  d, a
    add hl, de
    bit 7, e
    jr  nz, 09823$
    ld  de, #21
    add hl, de
09823$:
    ld  (_mxProbe), hl
    sra h
    rr  l
    sra h
    rr  l
    ld  (_mxCol), hl
    ld  de, (_mxOpenCol)
    or  a, a
    sbc hl, de
    jr  z, 09826$
    ld  de, (_mxProbe)
    ld  hl, (_game + OFF_PX)
    ld  a, (_mxStep)
    bit 7, a
    jr  nz, 09824$
    call _gg_feet3
    or  a, a
    jr  z, 09825$
    ld  a, #1
    ld  (_game + OFF_PGROUND), a
    jr  09827$
09824$:
    call _gg_head3
    or  a, a
    jr  z, 09825$
    ld  hl, #0x7FFF
    ld  (_game + OFF_PJUMPT), hl
09827$:
    ld  hl, #0
    ld  (_game + OFF_PVY), hl
    ret
09825$:
    ld  hl, (_mxCol)
    ld  (_mxOpenCol), hl
09826$:
    ld  a, (_mxStep)
    ld  e, a
    rlca
    sbc a, a
    ld  d, a
    ld  hl, (_game + OFF_PY)
    add hl, de
    ld  (_game + OFF_PY), hl
    ld  a, (_mxStep)
    ld  b, a
    ld  a, (_mxAmount)
    sub a, b
    ld  (_mxAmount), a
    jr  09822$
    __endasm;
}
#endif

/* ---------------------------------------------------------------- weapons */

#define JAZZ_NO_SECOND 0x7FFF

/* Per-type shot parameters from the level's bullet table. */
static const s16 shotVx[JAZZ_SHOT_TYPES]  = { 1000, 1400, 1400, 700, 1600 };
static const s16 shotVy[JAZZ_SHOT_TYPES]  = { 0, 0, -256, -768, 0 };
static const s16 shotVy2[JAZZ_SHOT_TYPES] = { JAZZ_NO_SECOND, JAZZ_NO_SECOND, 256,
                                              JAZZ_NO_SECOND, JAZZ_NO_SECOND };

/* 8.8 velocity plus running remainder -> whole pixels (truncates toward 0). */
static s8 consume_subpixels(s16 velocity, s16 *acc)
{
    s16 total = (s16)(velocity + *acc);
    s8 pixels;
    if (total >= 0) pixels = (s8)(total >> 8);
    else pixels = (s8)-(s8)((u16)(-total) >> 8);
    *acc = (s16)(total - ((s16)pixels << 8));
    return pixels;
}

static JazzBullet *free_bullet(void)
{
    u8 i;
    for (i = 0; i < JAZZ_MAX_BULLETS; i++)
        if (!g->bullets[i].active) return &g->bullets[i];
    return 0;
}

static u8 spawn_bullet(s16 vx, s16 vy, s8 gravity, u8 behaviour)
{
    JazzBullet *b = free_bullet();
    if (!b) return 0;
    b->active = 1;
    b->x = g->player.x + (g->player.facing ? PLAYER_W : -2);
    b->y = g->player.y + 9;
    b->vx = g->player.facing ? vx : (s16)-vx;
    b->vy = vy;
    b->subX = 0;
    b->subY = 0;
    b->gravity = gravity;
    b->behaviour = behaviour;
    b->hostile = 0;
    return 1;
}

static void fire_bullet(void)
{
    u8 t = g->player.shotType;
    s8 gravity = (t == JAZZ_SHOT_BOUNCER) ? 24 : 0;
    u8 behaviour = (t == JAZZ_SHOT_BOUNCER) ? 4 : 0;
    if (t >= JAZZ_SHOT_TYPES) t = 0;
    if (!spawn_bullet(shotVx[t], shotVy[t], gravity, behaviour)) return;
    if (shotVy2[t] != JAZZ_NO_SECOND) spawn_bullet(shotVx[t], shotVy2[t], gravity, behaviour);
    g->fireCooldown = 10;
    g->events |= JAZZ_EVENT_FIRE;
}

static void destroy_cell(u8 gridX, u8 gridY)
{
    u8 n = g->destroyedCount;
    if (n >= JAZZ_MAX_DESTROYED) return;
    g->destroyedX[n] = gridX;
    g->destroyedY[n] = gridY;
    g->destroyedCols[gridX >> 3] |= (u8)(1 << (gridX & 7));
    g->destroyedCount = (u8)(n + 1);
}

static void kill_enemy(JazzEnemy *e)
{
    e->active = 0;
    g->score += 100;
    g->events |= JAZZ_EVENT_KILL;
    mark_event_taken(e->gridX, e->gridY);
    if ((e->klass == JJ1_CLASS_BOSS) && !g->transitionTimer) {
        g->transitionTimer = 75;
        g->events |= JAZZ_EVENT_EXIT;
    }
}

static void hit_destructible(u8 gridX, u8 gridY, u8 strength)
{
    u8 i, free_slot = JAZZ_MAX_DESTRUCT_HITS;
    if (strength <= 1) { destroy_cell(gridX, gridY); return; }
    for (i = 0; i < JAZZ_MAX_DESTRUCT_HITS; i++) {
        JazzDestructHit *d = &g->destructHits[i];
        if (d->active && (d->gridX == gridX) && (d->gridY == gridY)) {
            d->hits++;
            if (d->hits >= strength) { d->active = 0; destroy_cell(gridX, gridY); }
            return;
        }
        if (!d->active && (free_slot == JAZZ_MAX_DESTRUCT_HITS)) free_slot = i;
    }
    if (free_slot == JAZZ_MAX_DESTRUCT_HITS) free_slot = 0;
    g->destructHits[free_slot].gridX = gridX;
    g->destructHits[free_slot].gridY = gridY;
    g->destructHits[free_slot].hits = 1;
    g->destructHits[free_slot].active = 1;
}

static void grant_item(const Jj1EventInfo *info, u8 gridX, u8 gridY);

static void bullet_hits_scenery(s16 x, s16 y)
{
    s16 gx = x >> 5, gy = y >> 5;
    const Jj1EventInfo *info;
    if ((gx < 0) || (gx > 255) || (gy < 0) || (gy > 63)) return;
    if (gg_cell_destroyed((u8)gx, (u8)gy)) return;
    info = cell_info(gx, gy);
    if ((info->klass == JJ1_CLASS_END) && !g->transitionTimer) {
        g->transitionTimer = 75;
        g->events |= JAZZ_EVENT_EXIT;
        return;
    }
    if ((info->klass == JJ1_CLASS_ITEM) && info->strength) {
        if (!jazz_event_taken((u8)gx, (u8)gy)) grant_item(info, (u8)gx, (u8)gy);
        return;
    }
    if (info->klass != JJ1_CLASS_DESTRUCT) return;
    hit_destructible((u8)gx, (u8)gy, info->strength);
}

/* ------------------------------------------------------------------- bird */

static const s16 birdVx[2] = { 1400, 1700 };
static const s16 birdVy[2] = { -250, -500 };

static void update_bird(void)
{
    JazzPlayer *p = &g->player;
    s16 targetX, targetY;
    u8 e;
    if (!p->bird) return;
    targetX = (s16)(p->facing ? (p->x - 20) : (p->x + 20));
    targetY = (s16)(p->y - 20);
    p->birdX = (s16)(p->birdX + ((targetX - p->birdX) >> 3));
    p->birdY = (s16)(p->birdY + ((targetY - p->birdY) >> 3));
    if (p->birdCooldown) { p->birdCooldown--; return; }
    for (e = 0; e < JAZZ_MAX_ENEMIES; e++) {
        JazzEnemy *en = &g->enemies[e];
        s16 dx, dy;
        u8 shot;
        if (!en->active) continue;
        if ((en->klass != JJ1_CLASS_ENEMY_WALK) && (en->klass != JJ1_CLASS_ENEMY_FLY)) continue;
        dx = (s16)(en->x - p->birdX);
        if (p->facing ? (dx < 0) : (dx > 0)) continue;
        if (dx < 0) dx = (s16)-dx;
        if (dx > 160) continue;
        dy = (s16)(en->y - p->birdY);
        if (dy < 0) dy = (s16)-dy;
        if (dy > 48) continue;
        for (shot = 0; shot < 2; shot++) {
            JazzBullet *b = free_bullet();
            if (!b) break;
            b->active = 1;
            b->x = p->birdX;
            b->y = p->birdY;
            b->vx = (s16)(p->facing ? birdVx[shot] : -birdVx[shot]);
            b->vy = birdVy[shot];
            b->subX = 0;
            b->subY = 0;
            b->gravity = 24;
            b->behaviour = 0;
            b->hostile = 0;
            p->birdCooldown = 30;
            g->events |= JAZZ_EVENT_FIRE;
        }
        break;
    }
}

/* ---------------------------------------------------------------- bullets */

static void enemy_hit_box(const JazzEnemy *e, s16 *bx, s16 *by, s16 *bw, s16 *bh)
{
    if (e->klass == JJ1_CLASS_BOSS) {
        *bw = 96;
        *bh = 56;
        *bx = (s16)(e->x + (JAZZ_ENEMY_W >> 1) - 48);
        *by = (s16)(e->y + JAZZ_ENEMY_H - 56);
        return;
    }
    *bx = e->x;
    *by = e->y;
    *bw = ENEMY_W;
    *bh = ENEMY_H;
}

static JazzBullet cb;
#define b (&cb)

/* One bullet, in the working copy cb.  Returns with b->active cleared when
 * the shot dies. */
static void step_bullet(void)
{
    u8 e;
    s8 dx, dy;
    if (b->gravity) b->vy = (s16)(b->vy + b->gravity);
    dx = consume_subpixels(b->vx, &b->subX);
    dy = consume_subpixels(b->vy, &b->subY);
    b->x = (s16)(b->x + dx);
    b->y = (s16)(b->y + dy);
    /* Crates are events, not solid tiles: test the event grid too. */
    {
        s16 cgx = b->x >> 5, cgy = b->y >> 5;
        if ((cgx >= 0) && (cgx < 256) && (cgy >= 0) && (cgy < 64)) {
            const Jj1EventInfo *ci = cell_info(cgx, cgy);
            if ((ci->klass == JJ1_CLASS_ITEM) && ci->strength &&
                !jazz_event_taken((u8)cgx, (u8)cgy)) {
                grant_item(ci, (u8)cgx, (u8)cgy);
                b->active = 0;
                return;
            }
        }
    }
    if (rect_hits_solid(b->x, b->y, 4, 4)) {
        if (b->behaviour == 4) {
            b->x = (s16)(b->x - dx);
            b->y = (s16)(b->y - dy);
            if (rect_hits_solid((s16)(b->x + dx), b->y, 4, 4)) b->vx = (s16)-b->vx;
            if (rect_hits_solid(b->x, (s16)(b->y + dy), 4, 4)) {
                b->vy = (s16)(-b->vy - (b->vy >> 2));
                if (b->vy > -200 && b->vy < 200) b->vy = -400;
            }
        } else {
            b->active = 0;
            bullet_hits_scenery((s16)(b->x + ((b->vx > 0) ? 3 : 0)), (s16)(b->y + 2));
            return;
        }
    }
    if ((b->x < g->player.x - 400) || (b->x > g->player.x + 400)) { b->active = 0; return; }
    if (b->hostile) {
        if (rect_overlap(b->x, b->y, 4, 4, g->player.x, g->player.y, PLAYER_W, PLAYER_H)) {
            b->active = 0;
            hurt_player();
        }
        return;
    }
    {
        JazzEnemy *en = g->enemies;
        for (e = 0; e < JAZZ_MAX_ENEMIES; e++, en++) {
            s16 hx, hy, hw, hh;
            if (!en->active) continue;
            if (en->klass == JJ1_CLASS_HAZARD) continue;
            enemy_hit_box(en, &hx, &hy, &hw, &hh);
            if (!rect_overlap(b->x, b->y, 4, 4, hx, hy, hw, hh)) continue;
            b->active = 0;
            if (en->hitPoints > 1) en->hitPoints--;
            else kill_enemy(en);
            break;
        }
    }
}
#undef b

static void update_bullets(void)
{
    u8 i;
    JazzBullet *bp = g->bullets;
    for (i = 0; i < JAZZ_MAX_BULLETS; i++, bp++) {
        if (!bp->active) continue;
        cb = *bp;
        step_bullet();
        *bp = cb;
    }
}

/* ---------------------------------------------------------------- enemies */

static const s8 jj1_sin64[64] = {
    0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 118, 122, 125, 127,
    127, 127, 125, 122, 118, 112, 106, 98, 90, 81, 71, 60, 49, 37, 25, 12,
    0, -12, -25, -37, -49, -60, -71, -81, -90, -98, -106, -112, -118, -122, -125, -127,
    -127, -127, -125, -122, -118, -112, -106, -98, -90, -81, -71, -60, -49, -37, -25, -12
};

const s8 jj1_bob[64] = {
      0,  2,  3,  5,  6,  8,  9, 10, 12, 13, 14, 14, 15, 16, 16, 16,
     16, 16, 16, 15, 14, 14, 13, 12, 10,  9,  8,  6,  5,  3,  2,  0,
      0, -2, -3, -5, -6, -8, -9,-10,-12,-13,-14,-14,-15,-16,-16,-16,
    -16,-16,-16,-15,-14,-14,-13,-12,-10, -9, -8, -6, -5, -3, -2,  0
};

void spawn_grid_enemy(u8 gx, u8 gy, u8 klass, u8 strength)
{
    u8 i;
    JazzEnemy *e = 0;
    u8 first = (u8)((klass == JJ1_CLASS_BOSS) ? 0 : (g->bossPresent ? JJ1_BOSS_SLOT_SPAN : 0));
    for (i = 0; i < JAZZ_MAX_ENEMIES; i++)
        if (g->enemies[i].active && (g->enemies[i].gridX == gx) && (g->enemies[i].gridY == gy))
            return;
    if (klass == JJ1_CLASS_BOSS) {
        if (g->enemies[0].active) return;
        e = &g->enemies[0];
    } else {
        for (i = first; i < JAZZ_MAX_ENEMIES; i++)
            if (!g->enemies[i].active) { e = &g->enemies[i]; break; }
    }
    if (!e) return;
    e->active = 1;
    e->klass = klass;
    e->gridX = gx;
    e->gridY = gy;
    e->homeX = ((s16)gx << 5) + ((32 - ENEMY_W) >> 1);
    e->homeY = ((s16)gy << 5) + (32 - ENEMY_H);
    e->x = e->homeX;
    e->y = e->homeY;
    e->direction = -1;
    e->hitPoints = strength ? strength : 1;
    e->phase = (u8)((gx * 7) & 63);
    e->bossFacing = 0;
    if (klass == JJ1_CLASS_ENEMY_WALK) {
        s16 fx = e->x + (ENEMY_W >> 1);
        while ((e->y < (WORLD_H - ENEMY_H - 1)) && !down_pt_solid(fx, e->y + ENEMY_H)) e->y++;
        e->homeY = e->y;
    }
}

#define JJ1_ACTIVATE_MARGIN_X 208
#define JJ1_ACTIVATE_MARGIN_Y 160
#define JJ1_DEACTIVATE_MARGIN 288

#ifndef GG_HOST
/* Z80 versions of the scan's cell loop and deactivation pass. */
u8 scanK, scanStr, scanGy;
s16 scanXl, scanYl;

void spawn_from_scan(void)
{
    spawn_grid_enemy(scanGx, scanGy, scanK, scanStr);
}

/* For each scanIds[i] (row scanGy0 + i of column scanGx): if its class is
 * walker / flyer / guardian / hazard and the cell is not taken, spawn. */
static void scan_cells_asm(void) __naked
{
    __asm
    ld  a, (_scanN)
    or  a, a
    ret z
    ld  b, a
    ld  a, (_scanGy0)
    ld  c, a
    ld  hl, #_scanIds
09901$:
    ld  a, (hl)
    inc hl
    or  a, a
    jr  z, 09909$
    push hl
    push bc
    ld  l, a
    ld  h, #0
    add hl, hl
    add hl, hl
    ld  de, #_gg_einfo
    add hl, de
    ld  a, (hl)
    cp  a, #2
    jr  z, 09902$
    cp  a, #3
    jr  z, 09902$
    cp  a, #12
    jr  z, 09902$
    cp  a, #4
    jr  nz, 09908$
09902$:
    ld  (_scanK), a
    inc hl
    inc hl
    inc hl
    ld  a, (hl)
    ld  (_scanStr), a
    ld  a, c
    ld  (_scanGy), a
    ld  l, c
    ld  a, (_scanGx)
    call _jazz_event_taken
    or  a, a
    jr  nz, 09908$
    call _spawn_from_scan
09908$:
    pop bc
    pop hl
09909$:
    inc c
    djnz 09901$
    ret
    __endasm;
}

/* Deactivate non-guardian enemies with (u16)(x - scanXl) > 576 or
 * (u16)(y - scanYl) > 576. */
static void deactivate_asm(void) __naked
{
    __asm
    push ix
    ld  ix, #(_game + 39)
    ld  b, #6
09921$:
    ld  a, 9 (ix)
    or  a, a
    jr  z, 09924$
    ld  a, 10 (ix)
    cp  a, #12
    jr  z, 09924$
    ld  l, 0 (ix)
    ld  h, 1 (ix)
    ld  de, (_scanXl)
    or  a, a
    sbc hl, de
    ld  de, #577
    or  a, a
    sbc hl, de
    jr  nc, 09923$
    ld  l, 2 (ix)
    ld  h, 3 (ix)
    ld  de, (_scanYl)
    or  a, a
    sbc hl, de
    ld  de, #577
    or  a, a
    sbc hl, de
    jr  c, 09924$
09923$:
    ld  9 (ix), #0
09924$:
    ld  de, #16
    add ix, de
    djnz 09921$
    pop ix
    ret
    __endasm;
}
#endif

static void scan_grid_enemies(void)
{
    s16 px = g->player.x;
    s16 py = g->player.y;
    s16 gx0 = (px - JJ1_ACTIVATE_MARGIN_X) >> 5;
    s16 gx1 = (px + JJ1_ACTIVATE_MARGIN_X) >> 5;
    s16 gy0 = (py - JJ1_ACTIVATE_MARGIN_Y) >> 5;
    s16 gy1 = (py + JJ1_ACTIVATE_MARGIN_Y) >> 5;
    u8 gx, gy, i, n, span;
    JazzEnemy *e;
    if (gx0 < 0) gx0 = 0;
    if (gy0 < 0) gy0 = 0;
    if (gx1 > 255) gx1 = 255;
    if (gy1 > 63) gy1 = 63;
    /* Round-robin, one activation column per frame: column
       enemyScanColumn % span as on the Genesis, kept as a running offset so
       the Z80 only divides when the span changes (near the map edges). */
    span = (u8)(gx1 - gx0 + 1);
    if (span != scanSpan) {
        scanSpan = span;
        scanOffset = (u8)(g->enemyScanColumn % span);
    }
    gx = (u8)(gx0 + scanOffset);
    g->enemyScanColumn++;
    scanOffset++;
    if (scanOffset >= span) scanOffset = 0;
    n = (u8)(gy1 - gy0 + 1);
    if (n > sizeof(scanIds)) n = sizeof(scanIds);
    scanGx = gx;
    scanGy0 = (u8)gy0;
    scanN = n;
    gg_scan_column();
#ifdef GG_HOST
    gy = (u8)gy0;
    for (i = 0; i < n; i++, gy++) {
        u8 id = scanIds[i], k;
        const Jj1EventInfo *info;
        if (!id) continue;
        info = gg_event_info(id);
        k = info->klass;
        if (((k == JJ1_CLASS_ENEMY_WALK) || (k == JJ1_CLASS_ENEMY_FLY) ||
             (k == JJ1_CLASS_BOSS) || (k == JJ1_CLASS_HAZARD)) &&
            !jazz_event_taken(gx, gy))
            spawn_grid_enemy(gx, gy, k, info->strength);
    }
#else
    scan_cells_asm();
#endif
    if (g->bossPresent && !jazz_event_taken(g->bossGridX, g->bossGridY)) {
        s16 bx = (s16)(((s16)g->bossGridX << 5) + 16);
        s16 by = (s16)(((s16)g->bossGridY << 5) + 16);
        if ((bx > px - 512) && (bx < px + 512) && (by > py - 384) && (by < py + 384)) {
            const Jj1EventInfo *info = cell_info(g->bossGridX, g->bossGridY);
            spawn_grid_enemy(g->bossGridX, g->bossGridY, info->klass, info->strength);
        }
    }
    {
        /* x outside [px-288, px+288]  <=>  (u16)(x - (px-288)) > 576, as all
           coordinates stay well inside the s16 range. */
        s16 xl = px - JJ1_DEACTIVATE_MARGIN;
        s16 yl = py - JJ1_DEACTIVATE_MARGIN;
#ifdef GG_HOST
        e = g->enemies;
        for (i = 0; i < JAZZ_MAX_ENEMIES; i++, e++) {
            if (!e->active) continue;
            if (e->klass == JJ1_CLASS_BOSS) continue;
            if (((u16)(e->x - xl) > (u16)(2 * JJ1_DEACTIVATE_MARGIN)) ||
                ((u16)(e->y - yl) > (u16)(2 * JJ1_DEACTIVATE_MARGIN)))
                e->active = 0;
        }
#else
        scanXl = xl;
        scanYl = yl;
        deactivate_asm();
        (void)e;
#endif
    }
}

static void grant_item(const Jj1EventInfo *info, u8 gridX, u8 gridY)
{
    u8 kind = (u8)(info->param & 0x0F);
    mark_event_taken(gridX, gridY);
    g->stageGems++;
    g->score += (u16)info->points * 25;
    g->events |= JAZZ_EVENT_GEM;
    switch (kind) {
    case JJ1_ITEM_HEALTH:
        if (g->health < 5) g->health++;
        g->events |= JAZZ_EVENT_HEALTH;
        break;
    case JJ1_ITEM_LIFE:
        if (g->lives < 9) g->lives++;
        g->events |= JAZZ_EVENT_LIFE;
        break;
    case JJ1_ITEM_AMMO: {
        u8 shot = (u8)((info->param >> 4) & 0x0F);
        if (shot && (shot < JAZZ_SHOT_TYPES)) {
            u16 total;
            if (!g->player.ammo[shot]) g->player.shotType = shot;
            g->player.weaponsOwned |= (u8)(1 << shot);
            total = (u16)(g->player.ammo[shot] + JAZZ_AMMO_PER_CRATE);
            g->player.ammo[shot] = (u8)((total > 255) ? 255 : total);
        }
        break;
    }
    case JJ1_ITEM_INVINCIBLE:
        g->player.invincibleTime = JAZZ_INVINCIBLE_FRAMES;
        break;
    case JJ1_ITEM_AIRBOARD:
        g->player.flying = 1;
        break;
    case JJ1_ITEM_BIRD:
        g->player.bird = 1;
        g->player.birdX = g->player.x;
        g->player.birdY = (s16)(g->player.y - 24);
        break;
    case JJ1_ITEM_SHIELD:
        g->player.shield = (u8)((info->param >> 4) & 0x0F);
        break;
    case JJ1_ITEM_FASTFEET:
        g->player.fastFeetTime = JAZZ_FASTFEET_FRAMES;
        break;
    case JJ1_ITEM_HIGHJUMP:
        g->player.highJump = 1;
        break;
    default:
        break;
    }
}

static void collect_grid_items(void)
{
    u8 i;
    gather_player_cells();
    for (i = 0; i < pcN; i++) {
        const Jj1EventInfo *info = gg_event_info(pcId[i]);
        if (info->klass == JJ1_CLASS_UNBOARD) { g->player.flying = 0; continue; }
        if (info->klass != JJ1_CLASS_ITEM) continue;
        if (jazz_event_taken(pcX[i], pcY[i])) continue;
        if (info->strength) continue;   /* crates must be shot open */
        grant_item(info, pcX[i], pcY[i]);
    }
}

/* SDCC addresses a static struct directly but goes through IX/IY for a
 * struct pointer, so each enemy (and bullet) is processed in a static working
 * copy and written back. */
static JazzEnemy ce;
#define e (&ce)

void update_boss(void)
{
    const Jj1EventInfo *bi = cell_info(e->gridX, e->gridY);
    u8 total = bi->strength ? bi->strength : 2;
    u8 spent = (u8)(total - e->hitPoints);
    s16 sn, cs;
    e->phase = (u8)((e->phase + 1) & 63);
    sn = jj1_sin64[e->phase];
    cs = jj1_sin64[(e->phase + 16) & 63];
    if (e->direction < 1 || e->direction > 9) e->direction = 1;

    if (spent < (u8)(total >> 1)) {
        /* Stage 0: the elliptical circuit, +-96 px by +-64 px (x*96>>7 is
           x*3/4, x*64>>7 is x/2). */
        s16 ox = (s16)((sn * 3) >> 2);
        s16 oy = (s16)(cs >> 1);
        if ((e->direction == 1) || (e->direction >= 4)) e->x = (s16)(e->homeX + ox + 96);
        else e->x = (s16)(e->homeX - ox - 96);
        e->y = (s16)(e->homeY - oy);
        switch (e->direction) {
        case 1: if (cs > 0) e->direction = 2; break;
        case 2: if (cs < 0) e->direction = 3; break;
        case 3: if (cs > 0) e->direction = 4; break;
        default: if (cs < 0) e->direction = 1; break;
        }
        e->bossFacing = (u8)(e->direction <= 2);
        if ((g->frame & 127) == 0) {
            JazzBullet *b = free_bullet();
            if (b) {
                b->active = 1;
                b->x = (s16)(e->x + ((e->direction >= 3) ? 87 : -73));
                b->y = (s16)(e->y - 3);
                b->vx = (s16)((e->direction >= 3) ? 1400 : -1400);
                b->vy = 0;
                b->subX = 0;
                b->subY = 0;
                b->gravity = 0;
                b->behaviour = 0;
                b->hostile = 1;
            }
        }
    } else {
        /* Stage 1: the drill charges along a lane and bores into the floor. */
        s16 lane = (s16)(e->homeY + 40);
        s16 amp = (s16)(cs < 0 ? -cs : cs);
        s16 centreX = (s16)(e->x + (JAZZ_ENEMY_W >> 1));
        if (e->direction < 5) e->direction = (e->y > lane) ? 5 : 6;
        if (e->direction == 5) {
            if (e->y > lane) e->y -= 2; else e->direction = 8;
        } else if (e->direction == 6) {
            if (e->y < lane) e->y += 2; else e->direction = 8;
        } else if (e->direction == 8) {
            s16 step = (s16)((amp * 6) >> 7);
            e->y = (s16)(lane - ((amp * 3) >> 2));
            e->x = (s16)(e->x + (e->bossFacing ? -step : step));
            if (e->x < e->homeX - 128) e->bossFacing = 0;
            if (e->x > e->homeX + 128) e->bossFacing = 1;
            if (cs < 0 && gg_down_solid_raw(centreX, (s16)(e->y + 32))) e->direction = 9;
        } else {
            e->x = (s16)(e->x + ((e->x & 1) ? 1 : -1));
            if (cs >= 0 && cs <= 12) {
                s16 bgx = (s16)(centreX >> 5);
                s16 bgy = (s16)((e->y >> 5) + 1);
                if ((bgx >= 0) && (bgx < 256) && (bgy >= 0) && (bgy < 64) &&
                    !gg_cell_destroyed((u8)bgx, (u8)bgy))
                    destroy_cell((u8)bgx, (u8)bgy);
                e->direction = 8;
            }
        }
    }
}

static void step_enemy(void)
{
    {
        if (e->klass == JJ1_CLASS_ENEMY_WALK) {
            s16 nx = e->x + e->direction;
            s16 footX = nx + ((e->direction > 0) ? (ENEMY_W - 1) : 0);
            if (pt_solid(footX, e->y + (ENEMY_H >> 1)) ||
                !down_pt_solid(footX, e->y + ENEMY_H + 2) ||
                (nx < e->homeX - 64) || (nx > e->homeX + 64))
                e->direction = -e->direction;
            else e->x = nx;
        } else if (e->klass == JJ1_CLASS_ENEMY_FLY) {
            e->phase = (e->phase + 1) & 63;
            e->y = e->homeY + jj1_bob[e->phase];
            e->x += e->direction;
            if ((e->x < e->homeX - 48) || (e->x > e->homeX + 48)) e->direction = -e->direction;
        } else if (e->klass == JJ1_CLASS_BOSS) {
            update_boss();
        }
        if (rect_overlap(g->player.x, g->player.y, PLAYER_W, PLAYER_H, e->x, e->y, ENEMY_W, ENEMY_H))
            hurt_player();
    }
}
#undef e

#ifdef GG_HOST
static void update_enemies(void)
{
    u8 i;
    JazzEnemy *ep = g->enemies;
    for (i = 0; i < JAZZ_MAX_ENEMIES; i++, ep++) {
        if (!ep->active) continue;
        ce = *ep;
        step_enemy();
        *ep = ce;
    }
}
#else
/* Z80 version of the loop above (walkers and flyers inline, the guardian
 * through step_enemy's C), with IX on the enemy.  JazzEnemy offsets:
 * x 0, y 2, homeX 4, homeY 6, direction 8, active 9, klass 10, phase 14.
 * The range tests are the C ones rewritten as single unsigned compares:
 *   nx outside [homeX-64, homeX+64]  <=>  (u16)(nx - homeX + 64) > 128
 *   x outside [homeX-48, homeX+48]   <=>  (u16)(x - homeX + 48) > 96
 *   overlap(14x22 player, 14x16 enemy) <=> (u16)(px-ex+13) < 27 &&
 *                                          (u16)(py-ey+21) < 37      */
s16 enx, efoot;
typedef char check_player_x_at_1[(offsetof(JazzGame, player.x) == 1) ? 1 : -1];
typedef char check_enemies_at_39[(offsetof(JazzGame, enemies) == 39) ? 1 : -1];
typedef char check_enemy_size[(sizeof(JazzEnemy) == 16) ? 1 : -1];

static void update_enemies(void) __naked
{
    __asm
    push ix
    ld  ix, #(_game + 39)
    ld  b, #6
09701$:
    push bc
    ld  a, 9 (ix)
    or  a, a
    jp  z, 09790$
    ld  a, 10 (ix)
    cp  a, #2
    jr  z, 09710$
    cp  a, #3
    jp  z, 09730$
    cp  a, #12
    jp  z, 09750$
    jp  09770$
09710$:
    ld  l, 0 (ix)
    ld  h, 1 (ix)
    ld  a, 8 (ix)
    ld  e, a
    rlca
    sbc a, a
    ld  d, a
    add hl, de
    ld  (_enx), hl
    bit 7, e
    jr  nz, 09711$
    ld  de, #13
    add hl, de
09711$:
    ld  (_efoot), hl
    ld  l, 2 (ix)
    ld  h, 3 (ix)
    ld  de, #8
    add hl, de
    ex  de, hl
    ld  hl, (_efoot)
    call _gg_solid
    or  a, a
    jr  nz, 09720$
    ld  l, 2 (ix)
    ld  h, 3 (ix)
    ld  de, #18
    add hl, de
    ex  de, hl
    ld  hl, (_efoot)
    call _gg_down_solid
    or  a, a
    jr  z, 09720$
    ld  hl, (_enx)
    ld  e, 4 (ix)
    ld  d, 5 (ix)
    or  a, a
    sbc hl, de
    ld  de, #64
    add hl, de
    ld  de, #129
    or  a, a
    sbc hl, de
    jr  nc, 09720$
    ld  hl, (_enx)
    ld  0 (ix), l
    ld  1 (ix), h
    jp  09770$
09720$:
    ld  a, 8 (ix)
    neg
    ld  8 (ix), a
    jp  09770$
09730$:
    ld  a, 14 (ix)
    inc a
    and a, #63
    ld  14 (ix), a
    ld  e, a
    ld  d, #0
    ld  hl, #_jj1_bob
    add hl, de
    ld  a, (hl)
    ld  e, a
    rlca
    sbc a, a
    ld  d, a
    ld  l, 6 (ix)
    ld  h, 7 (ix)
    add hl, de
    ld  2 (ix), l
    ld  3 (ix), h
    ld  a, 8 (ix)
    ld  e, a
    rlca
    sbc a, a
    ld  d, a
    ld  l, 0 (ix)
    ld  h, 1 (ix)
    add hl, de
    ld  0 (ix), l
    ld  1 (ix), h
    ld  e, 4 (ix)
    ld  d, 5 (ix)
    or  a, a
    sbc hl, de
    ld  de, #48
    add hl, de
    ld  de, #97
    or  a, a
    sbc hl, de
    jr  c, 09770$
    ld  a, 8 (ix)
    neg
    ld  8 (ix), a
    jr  09770$
09750$:
    push ix
    push ix
    pop hl
    ld  de, #_ce
    ld  bc, #16
    ldir
    call _update_boss
    pop ix
    push ix
    pop de
    ld  hl, #_ce
    ld  bc, #16
    ldir
09770$:
    ld  hl, (_game + 1)
    ld  e, 0 (ix)
    ld  d, 1 (ix)
    or  a, a
    sbc hl, de
    ld  de, #13
    add hl, de
    ld  de, #27
    or  a, a
    sbc hl, de
    jr  nc, 09790$
    ld  hl, (_game + 3)
    ld  e, 2 (ix)
    ld  d, 3 (ix)
    or  a, a
    sbc hl, de
    ld  de, #21
    add hl, de
    ld  de, #37
    or  a, a
    sbc hl, de
    jr  nc, 09790$
    call _hurt_player
09790$:
    ld  de, #16
    add ix, de
    pop bc
    dec b
    jp  nz, 09701$
    pop ix
    ret
    __endasm;
}
#endif

/* --------------------------------------------------------------- movement */

static s16 run_ceiling(void)
{
    return g->player.fastFeetTime ? JAZZ_PXS_FASTRUN : JAZZ_PXS_RUN;
}

static void accelerate_player(u16 input)
{
    JazzPlayer *p = &g->player;
    s16 ceil = run_ceiling();
    if (input & JAZZ_INPUT_RIGHT) {
        p->facing = 1;
        if (p->vx < 0) p->vx += JAZZ_PXA_REVERSE;
        else if (p->vx < JAZZ_PXS_WALK) p->vx += JAZZ_PXA_WALK;
        else if (p->vx < ceil) p->vx += JAZZ_PXA_RUN;
        if (p->vx > ceil) p->vx = ceil;
    } else if (input & JAZZ_INPUT_LEFT) {
        p->facing = 0;
        if (p->vx > 0) p->vx -= JAZZ_PXA_REVERSE;
        else if (p->vx > -JAZZ_PXS_WALK) p->vx -= JAZZ_PXA_WALK;
        else if (p->vx > -ceil) p->vx -= JAZZ_PXA_RUN;
        if (p->vx < -ceil) p->vx = -ceil;
    } else {
        if (p->vx > 0) { if (p->vx < JAZZ_PXA_STOP) p->vx = 0; else p->vx -= JAZZ_PXA_STOP; }
        else if (p->vx < 0) { if (p->vx > -JAZZ_PXA_STOP) p->vx = 0; else p->vx += JAZZ_PXA_STOP; }
    }
}

static void integrate_vertical(u16 input)
{
    JazzPlayer *p = &g->player;

    if (p->flying) {
        if (input & (JAZZ_INPUT_UP | JAZZ_INPUT_JUMP)) {
            if (p->vy > 0) p->vy -= JAZZ_PXA_REVERSE;
            else if (p->vy > -JAZZ_PXS_WALK) p->vy -= JAZZ_PXA_WALK;
            else if (p->vy > -JAZZ_PXS_RUN) p->vy -= JAZZ_PXA_RUN;
        } else if (input & JAZZ_INPUT_DOWN) {
            if (p->vy < 0) p->vy += JAZZ_PXA_REVERSE;
            else if (p->vy < JAZZ_PXS_WALK) p->vy += JAZZ_PXA_WALK;
            else if (p->vy < JAZZ_PXS_RUN) p->vy += JAZZ_PXA_RUN;
        } else {
            if (p->vy > JAZZ_PXA_WALK) p->vy -= JAZZ_PXA_WALK;
            else if (p->vy < -JAZZ_PXA_WALK) p->vy += JAZZ_PXA_WALK;
            else p->vy = 0;
        }
        p->jumpTargetY = JAZZ_NO_JUMP_TARGET;
        p->springJump = 0;
        return;
    }

    if ((input & JAZZ_INPUT_JUMP) && p->onGround &&
        (p->jumpTargetY == JAZZ_NO_JUMP_TARGET) &&
        !(g->previousInput & JAZZ_INPUT_JUMP)) {
        s16 bonus = p->vx;
        if (bonus < 0) bonus = -bonus;
        p->jumpTargetY = p->y - JAZZ_PYO_JUMP - (bonus >> 5);
        if (p->highJump) p->jumpTargetY -= JAZZ_HIGHJUMP_BONUS;
        p->springJump = 0;
        g->events |= JAZZ_EVENT_JUMP;
    }

    if (!(input & JAZZ_INPUT_JUMP) && p->onGround && !p->springJump)
        p->jumpTargetY = JAZZ_NO_JUMP_TARGET;

    if (p->jumpTargetY != JAZZ_NO_JUMP_TARGET) {
        if ((!(input & JAZZ_INPUT_JUMP) && !p->springJump) || (p->y <= p->jumpTargetY)) {
            p->jumpTargetY = JAZZ_NO_JUMP_TARGET;
            p->springJump = 0;
        } else {
            /* ascent = -(remaining + 64)/16 px/f, i.e. -((remaining+64) << 4)
               in 8.8, clamped to the jump speed (so no 32-bit math needed). */
            s16 remaining = p->y - p->jumpTargetY;
            if (remaining > 1000) remaining = 1000;
            p->vy = (s16)-((remaining + 64) << 4);
            if (p->vy < JAZZ_PYS_JUMP) p->vy = JAZZ_PYS_JUMP;
            return;
        }
    }

    if (p->vy < 0) {
        p->vy += JAZZ_PYA_GRAVITY;
        if (p->vy >= 0) p->vy = JAZZ_PYS_FALL;
    } else {
        p->vy = JAZZ_PYS_FALL;
    }
}

static u8 player_anim_state(u16 input)
{
    const JazzPlayer *p = &g->player;
    s16 speed = (p->vx < 0) ? -p->vx : p->vx;
    if (p->flying) return JJ1_PLAYER_BOARD;
    if (g->invulnerability && !p->onGround) return JJ1_PLAYER_HURT;
    if (p->springJump) return JJ1_PLAYER_SPRING;
    if (!p->onGround) return (p->vy < 0) ? JJ1_PLAYER_JUMP : JJ1_PLAYER_FALL;
    if (g->fireCooldown && speed < JAZZ_PXS_WALK) return JJ1_PLAYER_SHOOT;
    if (speed >= JAZZ_PXS_RUN) return JJ1_PLAYER_RUN;
    if (((p->vx > 0) && (input & JAZZ_INPUT_LEFT)) ||
        ((p->vx < 0) && (input & JAZZ_INPUT_RIGHT)))
        return JJ1_PLAYER_SKID;
    if (speed >= JAZZ_PXA_WALK) return JJ1_PLAYER_WALK;
    if (input & JAZZ_INPUT_DOWN) return JJ1_PLAYER_CROUCH;
    if (input & JAZZ_INPUT_UP) return JJ1_PLAYER_LOOKUP;
    return JJ1_PLAYER_STAND;
}

/* ------------------------------------------------------------------- step */

void jazz_step(u16 input)
{
    u16 pressed = input & ~g->previousInput;
    s8 dx, dy;
    g->events = JAZZ_EVENT_NONE;
    g->frame++;

    if (pressed & JAZZ_INPUT_START) { g->previousInput = input; g->paused = !g->paused; return; }
    if (g->paused || g->finished) { g->previousInput = input; return; }

    if (g->transitionTimer) {
        g->previousInput = input;
        g->transitionTimer--;
        if (!g->transitionTimer) {
            if ((u8)(g->stage + 1) < JAZZ_STAGE_COUNT) build_stage(g->stage + 1);
            else g->finished = 1;
        }
        return;
    }

    accelerate_player(input);
    {
        s16 pushX = 0, targetY = 0;
        u8 kind;
        gather_player_cells();
        kind = tube_push(&pushX, &targetY);
        g->player.inTube = (kind != 0);
        if (kind) {
            if (pushX) {
                s16 mag = (pushX < 0) ? -pushX : pushX;
                u8 against = ((pushX < 0) && (input & JAZZ_INPUT_RIGHT)) ||
                             ((pushX > 0) && (input & JAZZ_INPUT_LEFT));
                if (mag >= JAZZ_PXS_WALK || !against) {
                    g->player.vx = pushX;
                    if (pushX > 0) g->player.facing = 1;
                    else if (pushX < 0) g->player.facing = 0;
                }
            }
            if ((kind == 2) && (g->player.y > targetY)) {
                g->player.jumpTargetY = targetY;
                g->player.springJump = 1;
            }
        }
    }
    integrate_vertical(input);
    g->previousInput = input;

    if (pressed & JAZZ_INPUT_SWITCH) {
        u8 next = g->player.shotType;
        u8 tries;
        for (tries = 0; tries < JAZZ_SHOT_TYPES; tries++) {
            next++;
            if (next >= JAZZ_SHOT_TYPES) next = 0;
            if (g->player.weaponsOwned & (u8)(1 << next)) {
                g->player.shotType = next;
                break;
            }
        }
    }
    if ((pressed & JAZZ_INPUT_FIRE) && !g->fireCooldown) fire_bullet();
    if (g->fireCooldown) g->fireCooldown--;
    if (g->player.invincibleTime) g->player.invincibleTime--;
    if (g->player.fastFeetTime) g->player.fastFeetTime--;
    if (g->invulnerability) g->invulnerability--;

    dx = consume_subpixels(g->player.vx, &g->player.subX);
    if (dx) move_player_x(dx);
    dy = consume_subpixels(g->player.vy, &g->player.subY);
    if (dy) move_player_y(dy);
    else if (g->player.vy > 0)
        g->player.onGround = player_vertical_blocked(g->player.x, g->player.y + 1, 1);

    {
        s16 springTop = 0;
        u16 height;
        gather_player_cells();
        height = spring_height(&springTop);
        if (height) {
            g->player.jumpTargetY = springTop - (s16)height - PLAYER_H;
            g->player.springJump = 1;
            g->player.vy = JAZZ_PYS_JUMP;
            g->player.onGround = 0;
            g->events |= JAZZ_EVENT_JUMP;
        }
    }
    scan_grid_enemies();
    update_bird();
    update_bullets();
    update_enemies();
    collect_grid_items();

    gather_player_cells();
    if (touches_end()) {
        g->transitionTimer = 75;
        g->events |= JAZZ_EVENT_EXIT;
    }
    g->animState = player_anim_state(input);
}
