# Jazz Jackrabbit — Game Gear port

A Sega Game Gear port of haroldo-ok's Genesis/SGDK port of Jazz Jackrabbit, with
everything the Genesis cartridge has, in a 4 MB ROM:

* **all 10 episodes** — episodes 1-6, A, B, C (the CD-ROM ones) and the Christmas
  episode X: 66 stages, every one with its own tiles, sprites, palette, palette
  cycling, events, enemies, guardians and music;
* the **front end** — title screen, main menu, episode list with the episode
  pictures, planet-approach screens, level-clear / ending / game-over screens,
  instructions and setup (music, sound effects, button layout);
* the **3D bonus levels** (collect the red diamond in a stage to enter one at its
  end, or pick row Z of the episode list) with a perspective floor;
* **saved games** — 4 slots in cartridge RAM, plus the options;
* the sampled **sound effects**, converted to the PSG.

Background blocks and sprites are drawn at half size (JJ1's 32×32 blocks become
16×16) and the level layouts are unchanged, so the 160×144 screen shows the
same area of the level as the Genesis's 320×288.  The game logic is the Genesis
`game_core.c`, and is checked against it frame by frame (see *Tests*).

## Controls
| Game Gear | Action |
|---|---|
| D-pad | move (and steer the airboard) |
| 2 | jump |
| 1 | fire |
| Up + 1 | switch weapon (not while on the airboard) |
| Start | pause |
| 1 while paused | save the game (choose a slot) |

Setup can swap buttons 1 and 2.  In the menus: Up/Down choose, Start or 1 accepts, 2 goes back.
Bonus level: Left/Right turn, Up/Down run forwards/backwards, jump as usual; collect every gem
before the time runs out for an extra life.

Stage select: on the title screen or the main menu enter **Up Up Down Down Left Right Left Right 2 1**;
a `STAGE SELECT` line appears, Left/Right changes the stage (8-10 are the bonus levels), and the next
new game starts there.

## Building
Requires SDCC 4.2, devkitSMS (`DEVKIT`, default `~/.devkitsms`), Python 3 with Pillow and numpy,
the Genesis port's repository and the original game's data files (the CD-ROM version has all episodes).

    # 1. in the Genesis repository: build the episode packs from the game data
    python3 tools/build_packs.py generate --input /path/to/JAZZ
    # 2. here
    export JJ1_DATA=/path/to/JAZZ                                  # default ~/jj1data
    export JJ_GENESIS_PACKS=/path/to/jazz-jackrabbit-for-sega-genesis/packs   # default ../jazz-jackrabbit-for-sega-genesis/packs
    make                # jazz.gg (4 MB, Sega mapper, cartridge RAM)
    make jazz_trace.gg  # differential-test build

Episodes are taken from the packs that exist, so a build from the shareware data has episode 1 only.

## Layout
* `src/main.c` – game flow, main loop (fixed timestep), sky gradient (line interrupts), bank trampolines
* `src/gg_logic.c` – the game simulation, a port of the Genesis `game_core.c` (ROM bank 2)
* `src/gg_world.c` – banked level data access in assembly (collision masks, cell and event chunk maps)
* `src/render.c` – tile-cache map streaming, sprites, HUD, palette cycling
* `src/frontend.c`, `src/fe_fixed.c` – menus, pictures, text screens (bank 3); VRAM/ROM helpers, saves
* `src/bonus.c`, `src/bonus_fixed.c` – bonus levels (bank 4): the Genesis `bonus_core.c` simulation, the
  floor sampler and raster interrupts
* `src/mulfix.s` – replaces SDCC 4.2's 16×16→32 signed multiply, which gets the sign wrong
* `src/music.c`, `src/sound.c` – music player (frame interrupt), PSG sound effects
* `tools/convert.py` – converts every episode into `gen/data.bin` (halved tiles/sprites, baked collision);
  `tools/frontend.py`, `tools/bonus.py`, `tools/psgsfx.py`, `tools/psgmusic.py` do the menus, bonus levels,
  effects and music (`tools/psm.py` reads the songs, `tools/smpanalysis.py` and `tools/pitchcheck.py`
  measure the instruments)

ROM: code in banks 0-4, data in banks 5-220 (35 banks still free).  Each stage has a 16 KB bank of tables; a song's order table and
envelopes sit in one bank and its pattern segments wherever they fit; cell and event maps
are stored as 4×4-cell chunks; world tiles and each episode's sprites are linear pools.  Jazz is drawn in
10 colours shared by all stages, and each stage picks the other 5 sprite colours.

## Tests
`make test` runs the first four; all need the `smstest` emulator harness in `tools/smstest`.
* `tests/smoke.txt` – title → menus → play
* `tests/tour.sh` – starts all 66 stages through the menus and checks picture and tile cache
* `tests/bonus.sh` – plays a bonus level to the "complete" screen
* `tests/bgstress.sh` + `tools/bgverify.py` – random play in every stage, then every dumped screen is
  checked pixel by pixel against the level data
* `tests/diff/run.sh`, `tests/diff/rom.sh` (`make difftest`; `EP=1..10`, `STAGES`, `JJ_GENESIS`) –
  differential tests: the GG logic, on the host and on the Z80, against the Genesis game core, frame by frame
* `tests/music/run.sh` – the ROM's PSG output against the reference player (title, songs of several
  episodes, with effects)
* `tests/bench.tpl` / `tests/speed.tpl` (through `tools/mktest.py`) – cycle profiles and real-time speed

## Known limits
* Busy scenes run the simulation at full speed but draw at about 30 frames a second.
* A bonus level takes a few seconds to start (its floor tiles are built in RAM first), and its floor is
  drawn at 40×10 samples, refreshed on alternate rows.
* The Genesis cartridge's own cuts apply here too (text screens instead of the animated ending).

## Music
The soundtrack is converted from the **original MS-DOS songs** — Epic's PSM tracker modules from the
game data (the menu song, every world's song of all episodes, `BOSS`, `ENDLEVEL`, the bonus song) —
not from the Genesis FM versions.  `tools/psgmusic.py` (with the module reader `tools/psm.py`):

* plays each 4- or 8-channel module tick by tick (speed/tempo, volume column, slides, portamento,
  vibrato, arpeggio, note cut/delay, pattern loops…);
* measures every sample (`tools/smpanalysis.py`): pitch from the harmonics of its steady part (a
  two-way mismatch fit, which does not lock onto the fifth or the octave), chords, the sample's own
  pitch movement (bent guitars, falling toms, sweeps), noisiness; the pitch class is then checked
  against the song itself (`tools/pitchcheck.py`), and drums are told apart from pitched sounds by the
  same test — names only say which drum (periodic noise for kicks, white noise for snares and hats);
* plays every pitch exactly where the original is off the semitone grid (slides, vibrato, bends);
* sets volumes by perceived loudness: each sample's level per video frame, A-weighted, against the
  A-weighted level of the square wave or noise that plays it;
* plays chord samples as their notes on free channels (root first) rather than as arpeggios, and moves
  a bass by whole octaves into the PSG's range (its lowest note is A2), folding only the lowest notes;
* shares the 3 tone channels by role (lead > bass > chords > toms/effects) and loudness, with the
  lowest-priority voice on tone 2, which the sound effects borrow;
* writes a compact event stream; patterns that repeat are stored once.  Each song's order table and
  envelopes sit in one bank and its pattern segments in any banks (1–23 KB per song, 320 KB in all).

`src/music.c` plays it from the frame interrupt and is byte-for-byte equal to the Python reference
player.  Each stage plays the song the Genesis port assigns to it.
`tools/musicpreview.py SONG.PSM outdir` renders the original mix and the Game Gear version to WAV;
`tools/pitchcheck.py SONG.PSM` lists each instrument's measured pitch and how well it matches.

## Sound effects
`tools/psgsfx.py` converts the game's own sampled effects (`SOUNDS.000` and the weapon shots) to PSG
streams of 3 bytes a frame (tone period + volume, measured per video frame); `src/sound.c` plays them
on the tone channel the music lends, with the Genesis port's priorities.
