# Jazz Jackrabbit - Game Gear (devkitSMS / SDCC)
DEVKIT  ?= $(HOME)/.devkitsms
CC      := sdcc
IHX2SMS := $(DEVKIT)/bin/ihx2sms
SMSTEST := tools/smstest/smstest
PROG    := jazz

OPT     := --opt-code-speed --max-allocs-per-node 5000
CFLAGS  := -mz80 -DTARGET_GG -I$(DEVKIT)/include -Isrc -Igen --peep-file $(DEVKIT)/include/peep-rules.txt $(OPT) $(EXTRA)
# game logic runs from ROM bank 2 through the slot-2 window
BANKFLAGS := --codeseg BANK2 --constseg BANK2
BANK3FLAGS := --codeseg BANK3 --constseg BANK3
BANK4FLAGS := --codeseg BANK4 --constseg BANK4
LDFLAGS := -mz80 --no-std-crt0 --data-loc 0xC000 -Wl-b_BANK2=0x28000 -Wl-b_BANK3=0x38000 -Wl-b_BANK4=0x48000 -Wl-u

FIXED   := build/main.rel build/render.rel build/gg_world.rel build/sound.rel build/music.rel build/gg_fixed.rel build/fe_fixed.rel build/bonus_fixed.rel build/mulfix.rel
BANKED  := build/gg_logic.rel build/frontend.rel build/gg_bank3.rel build/bonus.rel build/gg_bank4.rel

all: $(PROG).gg

gen/gg_data.h gen/gg_fixed.c gen/gg_bank3.c gen/gg_bank4.c gen/data.bin: tools/convert.py tools/jjdata.py tools/psgmusic.py tools/psm.py tools/frontend.py tools/bonus.py tools/psgsfx.py
	python3 tools/convert.py

build/%.rel: src/%.c src/*.h gen/gg_data.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/gg_fixed.rel: gen/gg_fixed.c gen/gg_data.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/gg_logic.rel: src/gg_logic.c src/*.h gen/gg_data.h | build
	$(CC) $(CFLAGS) $(BANKFLAGS) -c $< -o $@

build/frontend.rel: src/frontend.c src/*.h gen/gg_data.h | build
	$(CC) $(CFLAGS) $(BANK3FLAGS) -c $< -o $@

build/gg_bank3.rel: gen/gg_bank3.c gen/gg_data.h | build
	$(CC) $(CFLAGS) $(BANK3FLAGS) -c $< -o $@

build/bonus.rel: src/bonus.c src/*.h gen/gg_data.h | build
	$(CC) $(CFLAGS) $(BANK4FLAGS) -c $< -o $@

build/gg_bank4.rel: gen/gg_bank4.c gen/gg_data.h | build
	$(CC) $(CFLAGS) $(BANK4FLAGS) -c $< -o $@

build/mulfix.rel: src/mulfix.s | build
	sdasz80 -plosgff -o $@ $<

build/crt0_sms.rel: | build
	cp $(DEVKIT)/lib/crt0_sms.rel build/ && touch build/crt0_sms.lst

build:
	mkdir -p build

.PHONY: build/$(PROG).ihx
build/$(PROG).ihx: build/crt0_sms.rel
build/$(PROG).ihx: $(FIXED) $(BANKED)
	$(CC) -o $@ $(LDFLAGS) build/crt0_sms.rel $(FIXED) \
	      $(DEVKIT)/lib/SMSlib_GG.lib $(DEVKIT)/lib/PSGlib.lib $(BANKED)

build/code.gg: build/$(PROG).ihx
	python3 tools/ramcheck.py build/$(PROG).map
	$(IHX2SMS) $< $@

$(PROG).gg: build/code.gg gen/data.bin tools/mkrom.py
	python3 tools/mkrom.py build/code.gg gen/data.bin gen/gg_data.h $@
	cp build/$(PROG).noi $(PROG).noi

$(SMSTEST): tools/smstest/smstest.c
	$(MAKE) -C tools/smstest

test: $(PROG).gg $(SMSTEST)
	$(SMSTEST) $(PROG).gg tests/smoke.txt
	tests/tour.sh
	tests/bonus.sh
	tests/bgstress.sh

clean:
	rm -rf build $(PROG).gg $(PROG).noi

.PHONY: all clean test

# Differential-test ROM: same game, generated inputs, per-frame trace records.
build/trace/main.rel: src/main.c src/*.h gen/gg_data.h tests/diff/diff_common.h
	mkdir -p build/trace
	$(CC) $(CFLAGS) -DGG_TRACE -c $< -o $@

.PHONY: build/trace/jazz.ihx
build/trace/jazz.ihx: build/crt0_sms.rel build/trace/main.rel $(filter-out build/main.rel,$(FIXED)) $(BANKED)
	$(CC) -o $@ $(filter-out -Wl-u,$(LDFLAGS)) build/crt0_sms.rel build/trace/main.rel $(filter-out build/main.rel,$(FIXED)) \
	      $(DEVKIT)/lib/SMSlib_GG.lib $(DEVKIT)/lib/PSGlib.lib $(BANKED)

jazz_trace.gg: build/trace/jazz.ihx gen/data.bin tools/mkrom.py
	$(IHX2SMS) $< build/trace/code.gg
	python3 tools/mkrom.py build/trace/code.gg gen/data.bin gen/gg_data.h $@
	cp build/trace/jazz.noi jazz_trace.noi

difftest: jazz_trace.gg $(SMSTEST)
	tests/diff/run.sh
	tests/diff/rom.sh
.PHONY: difftest
