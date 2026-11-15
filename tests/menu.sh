# menu_start EP STAGE: smstest lines that go title -> NEW GAME -> episode row EP
# and start STAGE through the stage-select cheat.  Sourced by the test scripts.
menu_start() {
  echo "symbols jazz.noi"; echo "run 90"
  echo "poke feCheat 1 w1"; echo "poke feCheatStage $2 w1"
  echo "hold start"; echo "run 3"; echo "hold none"; echo "run 30"
  echo "hold start"; echo "run 3"; echo "hold none"; echo "run 40"
  for i in $(seq 1 $1); do echo "hold down"; echo "run 3"; echo "hold none"; echo "run 12"; done
  echo "hold start"; echo "run 3"; echo "hold none"
  echo "waituntil mode == 1 900 w1"
}
stages_of() { [ $1 = 9 ] && echo "0 1 2" || echo "0 1 2 3 4 5 6"; }
