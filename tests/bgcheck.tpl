symbols jazz.noi
run 60
poke benchMode 1 w1
poke selectedStage 0 w1
hold start
run 3
hold none
waituntil mode == 1 200 w1
run 5
hold right
waituntil @frame >= 120 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s0_0.bin
poke @paused 0 w1
hold right
waituntil @frame >= 240 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s0_1.bin
poke @paused 0 w1
hold left
waituntil @frame >= 330 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s0_2.bin
poke @paused 0 w1
hold right
waituntil @frame >= 450 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s0_3.bin
poke @paused 0 w1
hold right
waituntil @frame >= 600 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s0_4.bin
poke @paused 0 w1
poke mode 2 w1
run 5
hold start
run 3
hold none
run 30
poke selectedStage 1 w1
hold start
run 3
hold none
waituntil mode == 1 200 w1
run 5
hold right
waituntil @frame >= 120 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s1_0.bin
poke @paused 0 w1
hold right
waituntil @frame >= 240 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s1_1.bin
poke @paused 0 w1
hold left
waituntil @frame >= 330 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s1_2.bin
poke @paused 0 w1
hold right
waituntil @frame >= 450 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s1_3.bin
poke @paused 0 w1
hold right
waituntil @frame >= 600 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s1_4.bin
poke @paused 0 w1
poke mode 2 w1
run 5
hold start
run 3
hold none
run 30
poke selectedStage 2 w1
hold start
run 3
hold none
waituntil mode == 1 200 w1
run 5
hold right
waituntil @frame >= 120 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s2_0.bin
poke @paused 0 w1
hold right
waituntil @frame >= 240 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s2_1.bin
poke @paused 0 w1
hold left
waituntil @frame >= 330 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s2_2.bin
poke @paused 0 w1
hold right
waituntil @frame >= 450 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s2_3.bin
poke @paused 0 w1
hold right
waituntil @frame >= 600 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s2_4.bin
poke @paused 0 w1
poke mode 2 w1
run 5
hold start
run 3
hold none
run 30
poke selectedStage 3 w1
hold start
run 3
hold none
waituntil mode == 1 200 w1
run 5
hold right
waituntil @frame >= 120 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s3_0.bin
poke @paused 0 w1
hold right
waituntil @frame >= 240 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s3_1.bin
poke @paused 0 w1
hold left
waituntil @frame >= 330 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s3_2.bin
poke @paused 0 w1
hold right
waituntil @frame >= 450 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s3_3.bin
poke @paused 0 w1
hold right
waituntil @frame >= 600 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s3_4.bin
poke @paused 0 w1
poke mode 2 w1
run 5
hold start
run 3
hold none
run 30
poke selectedStage 4 w1
hold start
run 3
hold none
waituntil mode == 1 200 w1
run 5
hold right
waituntil @frame >= 120 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s4_0.bin
poke @paused 0 w1
hold right
waituntil @frame >= 240 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s4_1.bin
poke @paused 0 w1
hold left
waituntil @frame >= 330 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s4_2.bin
poke @paused 0 w1
hold right
waituntil @frame >= 450 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s4_3.bin
poke @paused 0 w1
hold right
waituntil @frame >= 600 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s4_4.bin
poke @paused 0 w1
poke mode 2 w1
run 5
hold start
run 3
hold none
run 30
poke selectedStage 5 w1
hold start
run 3
hold none
waituntil mode == 1 200 w1
run 5
hold right
waituntil @frame >= 120 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s5_0.bin
poke @paused 0 w1
hold right
waituntil @frame >= 240 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s5_1.bin
poke @paused 0 w1
hold left
waituntil @frame >= 330 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s5_2.bin
poke @paused 0 w1
hold right
waituntil @frame >= 450 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s5_3.bin
poke @paused 0 w1
hold right
waituntil @frame >= 600 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s5_4.bin
poke @paused 0 w1
poke mode 2 w1
run 5
hold start
run 3
hold none
run 30
poke selectedStage 6 w1
hold start
run 3
hold none
waituntil mode == 1 200 w1
run 5
hold right
waituntil @frame >= 120 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s6_0.bin
poke @paused 0 w1
hold right
waituntil @frame >= 240 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s6_1.bin
poke @paused 0 w1
hold left
waituntil @frame >= 330 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s6_2.bin
poke @paused 0 w1
hold right
waituntil @frame >= 450 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s6_3.bin
poke @paused 0 w1
hold right
waituntil @frame >= 600 3000
poke @paused 1 w1
hold none
run 40
dumpstate tests/bg/OUT_s6_4.bin
poke @paused 0 w1
poke mode 2 w1
run 5
hold start
run 3
hold none
run 30
