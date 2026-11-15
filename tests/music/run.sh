#!/usr/bin/env bash
# Music regression: the ROM's PSG writes must equal tools/psgmusic.py's
# reference player -- on the title screen, in stages of several episodes,
# and with sound effects borrowing tone 2 / noise.  Needs the PSM files in
# $JJ1_DATA.  Stages are reached through the menus with the stage-select
# cheat switched on (feCheat / feCheatStage).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
D="${JJ1_DATA:-$HOME/jj1data}"
B="$ROOT/build/musictest"; mkdir -p "$B"
cd "$ROOT"
SMS=tools/smstest/smstest
printf 'symbols jazz.noi\npsglog %s/title.log\nrun 3000\npsgstop\n' "$B" > "$B/title.txt"
$SMS jazz.gg "$B/title.txt" > /dev/null
echo -n "title:              "; python3 tests/music/check.py "$B/title.log" "$D/MENUSNG.PSM"

# start episode row $1, stage $2 (through title, menu, episode list, planet)
start() {
    echo "symbols jazz.noi"; echo "run 60"
    echo "poke feCheat 1 w1"; echo "poke feCheatStage $2 w1"
    echo "hold start"; echo "run 3"; echo "hold none"; echo "run 30"
    echo "hold start"; echo "run 3"; echo "hold none"; echo "run 40"
    for i in $(seq 1 "$1"); do echo "hold down"; echo "run 3"; echo "hold none"; echo "run 12"; done
    echo "hold start"; echo "run 3"; echo "hold none"; echo "run 200"
}

for t in 0:0:SONG0 0:2:SONG3 0:4:SONG17 0:6:BOSS 1:0:SONG7 3:2:SONG5 6:0:SONGCD0 8:4:SONGCD8 9:0:XM2; do
    IFS=: read -r ep st song <<< "$t"
    { start "$ep" "$st"; echo "psglog $B/stage.log"; echo "run 2500"; echo "psgstop"; } > "$B/s.txt"
    $SMS jazz.gg "$B/s.txt" > /dev/null
    printf "episode %d stage %d %-8s " $((ep + 1)) $((st + 1)) "$song:"
    python3 tests/music/check.py "$B/stage.log" "$D/$song.PSM" --sync
done

{ start 0 0; echo "psglog $B/sfx.log"
  echo "hold right+2"; echo "run 200"; echo "hold right+1"; echo "run 150"
  echo "hold left+2+1"; echo "run 200"; echo "hold 1"; echo "run 100"
  echo "hold none"; echo "run 400"; echo "psgstop"; } > "$B/sfx.txt"
$SMS jazz.gg "$B/sfx.txt" > /dev/null
echo -n "with effects:       "; python3 tests/music/check.py "$B/sfx.log" "$D/SONG0.PSM" --sfx
