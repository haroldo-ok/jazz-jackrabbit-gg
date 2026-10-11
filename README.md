# Jazz Jackrabbit — Game Gear port

A Sega Game Gear port of haroldo-ok's Genesis/SGDK homebrew port of Jazz
Jackrabbit (episode 1: Diamondus, Tubelectric, Medivo — 7 stages).
Background blocks and sprites are drawn at half size (JJ1's 32×32 blocks become
16×16), and the level layouts are unchanged, so the 160×144 screen shows the
same area of the level as the Genesis's 320×288.

## Controls
| Game Gear | Action |
|---|---|
| D-pad | move (and steer the airboard) |
| 2 | jump |
| 1 | fire |
| Up + 1 | switch weapon (not while on the airboard) |
| Start | pause; on the title screen, start (Left/Right picks the level) |

## Building
Requires SDCC 4.2 and devkitSMS (`DEVKIT`, default `~/.devkitsms`) plus Python 3 with Pillow.
The Genesis sources (`res/*.inc`) are read by `tools/convert.py`.

    make              # jazz.gg (1 MB, Sega mapper)
    make jazz_trace.gg  # differential-test build

## Layout
* `src/main.c` – main loop (fixed timestep: one game frame per elapsed video frame), title, sky gradient (line interrupts)
* `src/gg_logic.c` – the game simulation, a port of the Genesis `game_core.c`, run from ROM bank 2
* `src/gg_world.c` – banked level data access (collision masks, events)
* `src/render.c` – tile-cache map streaming, sprites, HUD
* `src/sound.c` – PSG sound effects
* `tools/convert.py` – converts the Genesis assets into `gen/data.bin` (halved tiles/sprites, baked collision)

## Tests
* `tests/diff/run.sh`, `tests/diff/rom.sh` – differential tests: the GG logic (on the host and on the Z80
  through the `smstest` emulator harness) against the Genesis game core, frame by frame
* `tests/bgstress.tpl` + `tools/bgverify.py` – random play, then every dumped screen is checked against the level data
* `tests/bench.tpl` / `tests/speed.tpl` – cycle profiles and real-time speed

There is no music: the Genesis tracks are YM2612 FM, which the Game Gear's PSG cannot play.
