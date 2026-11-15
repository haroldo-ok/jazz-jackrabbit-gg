#!/bin/bash
# Bonus level: start it from the episode list (last row), put Jazz next to the
# last gem, run into it and expect the gem count to reach the total and the
# "bonus level complete" screen.  Usage: tests/bonus.sh [rom]
cd "$(dirname "$0")/.."; ROM=${1:-jazz.gg}; mkdir -p shots build
RO=$((16#$(grep " _renderOverlay " ${ROM%.gg}.noi | awk '{print $3}' | sed 's/0x//')))
a() { printf "0x%X" $((RO + $1)); }   # field of the Bonus struct (src/bonus.c)
cat > build/bonus.txt <<EOT
symbols ${ROM%.gg}.noi
run 90
hold start
run 3
hold none
run 30
hold start
run 3
hold none
run 60
hold up
run 3
hold none
run 20
hold start
run 3
hold none
run 30
poke $(a 10) 16384
poke $(a 12) 12
poke $(a 14) 16384
poke $(a 16) 7
poke $(a 34) 256
poke $(a 40) 29
run 10
hold up
run 120
hold none
expectvdp display 1
expectcolors 6
screenshot shots/bonus_play.ppm
expect $(a 40) == 30 w1
run 250
screenshot shots/bonus_won.ppm
EOT
tools/smstest/smstest $ROM build/bonus.txt 2>&1 | tail -1
