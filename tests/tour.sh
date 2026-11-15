#!/bin/bash
# Start every stage of every episode through the menus, walk right, take a
# screenshot (shots/epE_sS.ppm) and check the display is on, the picture has
# colours and the tile cache never overflowed.  Usage: tests/tour.sh [rom]
cd "$(dirname "$0")/.."; . tests/menu.sh
ROM=${1:-jazz.gg}; mkdir -p shots build; fail=0; n=0
for ep in 0 1 2 3 4 5 6 7 8 9; do
 for st in $(stages_of $ep); do
  { menu_start $ep $st
    echo "run 60"; echo "expectvdp display 1"; echo "expectcolors 5"
    echo "hold right"; echo "run 120"; echo "hold none"; echo "run 20"
    echo "screenshot shots/ep${ep}_s${st}.ppm"
    echo "expect cacheFull == 0 w1"; } > build/tour.txt
  if tools/smstest/smstest $ROM build/tour.txt 2>&1 | grep -q "RESULT: ALL PASSED"; then n=$((n+1))
  else echo "FAILED: episode $ep stage $st"; fail=1; fi
 done
done
echo "$n stages passed"; exit $fail
