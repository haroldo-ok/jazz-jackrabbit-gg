symbols jazz.noi
run 60
poke selectedStage 0 w1
hold start
run 3
hold none
waituntil mode == 1 200 w1
run 30
psglog build/psg_sfx.log
hold right+2
run 200
hold right+1
run 150
hold left+2+1
run 200
hold 1
run 100
hold none
run 400
psgstop
