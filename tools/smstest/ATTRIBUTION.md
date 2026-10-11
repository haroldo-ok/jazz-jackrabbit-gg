# Attribution — smstest headless emulator

This directory bundles a minimal headless Sega Master System emulator used to
playtest homebrew ROMs automatically (no GUI, no human).

- `z80.c`, `z80.h` — Z80 CPU core by **superzazu**, MIT license
  (https://github.com/superzazu/z80).
- `smstest.c` — SMS wrapper (Sega mapper, VDP mode-4 renderer, joypad, PSG
  sink) plus the playtest script interpreter, by **Haroldo-OK**, taken from the
  reference project `jill-of-the-jungle-sms`
  (https://github.com/haroldo-ok/jill-of-the-jungle-sms).

These are bundled as build-time developer tooling. Check the upstream projects
for the authoritative, current license terms before redistributing.

The profiler, `pause` (NMI), `psglog`/`psgstop`, `dumpstate`, and the
8-sprites-per-scanline / BG-priority screenshot fidelity were upstreamed from the
harness in Haroldo-OK's `flashback-sms` port (itself built on this skill).
`prof_report.py` was added alongside to attribute profile buckets to symbols.

Cartridge RAM, the call-graph profiler (`cgstart`/`cgstop`/`cgdump` + `prof2.py`,
with the Z80 core's call/return hooks), `expectpace`, `spmin`, `hooklog`,
`shots`, `frames`, `timers` and the debug ports 0x04/0x06 come from the harness
in Haroldo-OK's `out-of-this-world-sms`; garbage-on-power-up RAM from
`flashback-sms`. The `expectpace` threshold default was changed from 28 to 27
cycles (`pacemin`, `SMSTEST_PACE`) to match devkitSMS's own RST 18h spacing.
