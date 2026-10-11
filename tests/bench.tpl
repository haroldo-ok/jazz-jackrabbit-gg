symbols jazz.noi
run 60
poke benchMode 1 w1
hold start
run 3
hold none
waituntil mode == 1 100 w1
run 30
hold right
waituntil @frame >= 100 2000
cgstart
waituntil @frame >= 300 3000
cgstop
cgdump build/cg.out
