#!/usr/bin/env python3
"""Emit JazzGame field offsets as SDCC lays them out (compiles a probe file
with sdcc -S and reads back the .dw values).  Usage: offsets.py > gen/offsets.txt"""
import os, re, subprocess, sys, tempfile
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FIELDS = ["destroyedCount", "player.x", "player.y", "player.vx", "player.vy",
          "player.onGround", "player.facing", "player.shotType", "player.flying",
          "player.weaponsOwned", "player.shield",
          "enemies", "bullets", "taken", "destroyedX", "destroyedY", "frame", "score",
          "stage", "lives", "health", "stageGems", "stageGemTotal", "paused",
          "transitionTimer", "finished", "animState", "invulnerability", "bossPresent"]
src = ['#include "jazz_game.h"', '#include <stddef.h>']
for i, f in enumerate(FIELDS):
    src.append("const unsigned int off_%d = offsetof(JazzGame, %s);" % (i, f))
src.append("const unsigned int size_enemy = sizeof(JazzEnemy);")
src.append("const unsigned int size_bullet = sizeof(JazzBullet);")
src.append("const unsigned int size_game = sizeof(JazzGame);")
d = tempfile.mkdtemp(dir=os.environ.get("TMPDIR"))
c = os.path.join(d, "o.c")
open(c, "w").write("\n".join(src) + "\n")
subprocess.check_call(["sdcc", "-mz80", "-S", "-I" + os.path.join(ROOT, "src"), c, "-o", os.path.join(d, "o.asm")])
asm = open(os.path.join(d, "o.asm")).read()
vals = re.findall(r"^_(\w+):\s*\n\s*\.dw\s+#(0x[0-9a-fA-F]+)", asm, re.M)
names = {("off_%d" % i): f for i, f in enumerate(FIELDS)}
for n, v in vals:
    print("%s %d" % (names.get(n, n), int(v, 16)))
