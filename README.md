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
* `src/music.c` – music player (frame interrupt)
* `src/sound.c` – PSG sound effects
* `tools/convert.py` – converts the Genesis assets into `gen/data.bin` (halved tiles/sprites, baked collision)

## Tests
* `tests/diff/run.sh`, `tests/diff/rom.sh` – differential tests: the GG logic (on the host and on the Z80
  through the `smstest` emulator harness) against the Genesis game core, frame by frame
* `tests/bgstress.tpl` + `tools/bgverify.py` – random play, then every dumped screen is checked against the level data
* `tests/bench.tpl` / `tests/speed.tpl` – cycle profiles and real-time speed
* `tests/music/run.sh` – the ROM's PSG output against the reference player (title, every song, with effects)

## Music
The soundtrack is converted from the **original MS-DOS songs** — Epic's PSM tracker modules from the
shareware (`MENUSNG`, `SONG0` Diamondus, `SONG3` Tubelectric, `SONG17` Medivo, `BOSS`, `ENDLEVEL`) —
not from the Genesis FM versions.  `tools/psgmusic.py` (with the module reader `tools/psm.py`):

* plays each 4-channel module tick by tick (speed/tempo, volume column, slides, portamento, note cut/delay…);
* turns every note into a PSG voice: the pitch from the sample's measured root (all JJ1 samples are rooted
  on C), bass moved up into the PSG's range (its lowest note is A2), chord samples (`majorwec`, `CHORma`…)
  played as fast arpeggios, drums on the noise channel (periodic noise for kicks, white for snares/hats);
* gives each voice a volume envelope measured from the sample itself (RMS per video frame at the note's
  playback rate), so decays, swells and reversed cymbals keep their shape;
* shares the 3 tone channels by priority (lead > bass > chords > toms) with the lowest-priority voice on
  tone 2, which the sound effects borrow; the music keeps running silently there and takes it back;
* writes a compact event stream (repeated patterns stored once; 0.6–10 KB per song, one ROM bank each).

`src/music.c` plays it from the frame interrupt (about 2,200 cycles a frame) and is byte-for-byte equal
to the Python reference player.  Title: MENUSNG; Diamondus/Tubelectric/Medivo stages: their world's song;
the boss stage: BOSS; episode complete: the ENDLEVEL jingle.  The converter reads the `*.PSM` files from
`$JJ1_DATA` (default `~/jj1data`, e.g. extracted from the shareware); without them the ROM is built silent.
`tools/musicpreview.py SONG.PSM outdir` renders the original mix and the Game Gear version to WAV.
