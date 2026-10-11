/* GG logic on the host: prints one DIFF_REC record per frame. */
#include <stdio.h>
#include <stdlib.h>
#include "jazz_game.h"
#include "diff_common.h"
JazzGame game;
void host_world_init(const char *path);
int main(int argc, char **argv)
{
    int stage = argc > 2 ? atoi(argv[2]) : 0, frames = argc > 3 ? atoi(argv[3]) : 3000, f, i;
    host_world_init(argv[1]);
    if (argc > 4) diffLfsr = (unsigned short)strtoul(argv[4], 0, 0);
    jazz_game_init();
    if (stage) jazz_set_stage((u8)stage);
    game.previousInput = JAZZ_INPUT_START;
    for (f = 0; f < frames; f++) {
        unsigned char rec[DIFF_REC];
        unsigned char nen = 0, nb = 0; unsigned short es = 0;
        jazz_step(diff_next_input());
        for (i = 0; i < JAZZ_MAX_ENEMIES; i++)
            if (game.enemies[i].active) { nen++; es = (unsigned short)(es + game.enemies[i].x * 3 + game.enemies[i].y); }
        for (i = 0; i < JAZZ_MAX_BULLETS; i++) if (game.bullets[i].active) nb++;
        diff_pack(rec, game.frame, game.player.x, game.player.y, game.player.vx, game.player.vy,
                  game.score, game.health, game.lives, game.stage, (unsigned char)game.stageGems, nen, es, nb);
        if (getenv("DIFF_VERBOSE") && f >= atoi(getenv("DIFF_VERBOSE")) - 3 && f <= atoi(getenv("DIFF_VERBOSE")) + 1) {
            fprintf(stderr, "f%d in=%04x p(%d,%d) inv=%d hp=%d\n", f, diffInput, game.player.x, game.player.y, game.invulnerability, game.health);
            for (i = 0; i < JAZZ_MAX_BULLETS; i++) if (game.bullets[i].active)
                fprintf(stderr, "   b%d (%d,%d) v(%d,%d) host=%d beh=%d\n", i, game.bullets[i].x, game.bullets[i].y, game.bullets[i].vx, game.bullets[i].vy, game.bullets[i].hostile, game.bullets[i].behaviour);
            for (i = 0; i < JAZZ_MAX_ENEMIES; i++) if (game.enemies[i].active)
                fprintf(stderr, "   e%d k%d (%d,%d) dir=%d hp=%d ph=%d\n", i, game.enemies[i].klass, game.enemies[i].x, game.enemies[i].y, game.enemies[i].direction, game.enemies[i].hitPoints, game.enemies[i].phase);
        }
        fwrite(rec, 1, DIFF_REC, stdout);
        if (game.finished) break;
    }
    return 0;
}
