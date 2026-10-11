/* Genesis game_core.c (the reference) on the host, same records. */
#include <stdio.h>
#include <stdlib.h>
#include "jazz_game.h"
#include "jj1_events.h"
#include "jj1_runtime.h"
#include "diff_common.h"
static JazzGame g;
int main(int argc, char **argv)
{
    int stage = argc > 1 ? atoi(argv[1]) : 0, frames = argc > 2 ? atoi(argv[2]) : 3000, f, i;
    if (argc > 3) diffLfsr = (unsigned short)strtoul(argv[3], 0, 0);
    jazz_game_init(&g);
    if (stage) jazz_debug_set_stage(&g, (u8)stage);
    g.previousInput = JAZZ_INPUT_START;
    for (f = 0; f < frames; f++) {
        unsigned char rec[DIFF_REC];
        unsigned char nen = 0, nb = 0; unsigned short es = 0;
        jazz_step(&g, diff_next_input());
        for (i = 0; i < JAZZ_MAX_ENEMIES; i++)
            if (g.enemies[i].active) { nen++; es = (unsigned short)(es + g.enemies[i].x * 3 + g.enemies[i].y); }
        for (i = 0; i < JAZZ_MAX_BULLETS; i++) if (g.bullets[i].active) nb++;
        diff_pack(rec, g.frame, g.player.x, g.player.y, g.player.vx, g.player.vy,
                  g.score, g.health, g.lives, g.stage, g.stageGems, nen, es, nb);
        if (getenv("DIFF_VERBOSE") && f >= atoi(getenv("DIFF_VERBOSE")) - 3 && f <= atoi(getenv("DIFF_VERBOSE")) + 1) {
            fprintf(stderr, "f%d in=%04x p(%d,%d) inv=%d hp=%d\n", f, diffInput, g.player.x, g.player.y, g.invulnerability, g.health);
            for (i = 0; i < JAZZ_MAX_BULLETS; i++) if (g.bullets[i].active)
                fprintf(stderr, "   b%d (%d,%d) v(%d,%d) host=%d beh=%d\n", i, g.bullets[i].x, g.bullets[i].y, g.bullets[i].vx, g.bullets[i].vy, g.bullets[i].hostile, g.bullets[i].behaviour);
            for (i = 0; i < JAZZ_MAX_ENEMIES; i++) if (g.enemies[i].active)
                fprintf(stderr, "   e%d k%d (%d,%d) dir=%d hp=%d ph=%d\n", i, g.enemies[i].klass, g.enemies[i].x, g.enemies[i].y, g.enemies[i].direction, g.enemies[i].hitPoints, g.enemies[i].phase);
        }
        fwrite(rec, 1, DIFF_REC, stdout);
        if (g.finished) break;
    }
    return 0;
}
