#!/bin/bash
# Random play in every stage, pausing now and then to dump the machine state;
# tools/bgverify.py then checks every dumped screen against the level data
# (catches tile-cache leaks).  Usage: tests/bgstress.sh [rom] [dumps per stage]
cd "$(dirname "$0")/.."; . tests/menu.sh
ROM=${1:-jazz.gg}; N=${2:-4}; mkdir -p tests/bg build; rm -f tests/bg/*.bin; fail=0
python3 tools/offsets.py > gen/offsets.txt
RANDOM=1234
keys=(right right right left up down "right 2" "right 1" "left 2" 2 1 none)
for ep in 0 1 2 3 4 5 6 7 8 9; do
 for st in $(stages_of $ep); do
  { menu_start $ep $st
    for d in $(seq 1 $N); do
      for k in 1 2 3 4 5 6 7 8; do
        echo "poke @lives 9 w1"; echo "poke @health 5 w1"
        echo "hold ${keys[$((RANDOM % ${#keys[@]}))]}"; echo "run $((20 + RANDOM % 60))"
      done
      echo "hold none"; echo "poke @paused 1 w1"; echo "run 40"
      echo "dumpstate tests/bg/e${ep}s${st}_$d.bin"; echo "poke @paused 0 w1"
    done; } > build/bgstress.tpl
  python3 tools/mktest.py build/bgstress.tpl > build/bgstress.txt
  tools/smstest/smstest $ROM build/bgstress.txt 2>&1 | grep -q "RESULT: ALL PASSED" || { echo "FAILED to run: episode $ep stage $st"; fail=1; }
 done
done
python3 tools/bgverify.py tests/bg/*.bin | grep -v ": OK$"; [ ${PIPESTATUS[0]} = 0 ] || fail=1
echo "$(ls tests/bg/*.bin | wc -l) screens checked"; exit $fail
