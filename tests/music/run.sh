#!/usr/bin/env bash
# Music regression: the ROM's PSG writes must equal tools/psgmusic.py's
# reference player -- on the title screen, in one stage per song, and with
# sound effects borrowing tone 2 / noise.  Needs the PSM files in $JJ1_DATA.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
D="${JJ1_DATA:-$HOME/jj1data}"
B="$ROOT/build/musictest"; mkdir -p "$B"
cd "$ROOT"
SMS=tools/smstest/smstest
printf 'symbols jazz.noi\npsglog %s/title.log\nrun 3000\npsgstop\n' "$B" > "$B/title.txt"
$SMS jazz.gg "$B/title.txt" > /dev/null
echo -n "title:        "; python3 tests/music/check.py "$B/title.log" "$D/MENUSNG.PSM"
for pair in 0:SONG0 2:SONG3 4:SONG17 6:BOSS; do
    st=${pair%%:*}; song=${pair##*:}
    cat > "$B/s.tpl" <<T
symbols jazz.noi
run 60
poke selectedStage $st w1
hold start
run 3
hold none
waituntil mode == 1 200 w1
run 30
psglog $B/stage.log
run 2500
psgstop
T
    python3 tools/mktest.py "$B/s.tpl" > "$B/s.txt"
    $SMS jazz.gg "$B/s.txt" > /dev/null
    echo -n "stage $((st+1)) $song: "; python3 tests/music/check.py "$B/stage.log" "$D/$song.PSM" --sync
done
sed "s#build/psg_sfx.log#$B/sfx.log#" tests/music/sfx.tpl > "$B/sfx.tpl"
python3 tools/mktest.py "$B/sfx.tpl" > "$B/sfx.txt"
$SMS jazz.gg "$B/sfx.txt" > /dev/null
echo -n "with effects: "; python3 tests/music/check.py "$B/sfx.log" "$D/SONG0.PSM" --sfx
