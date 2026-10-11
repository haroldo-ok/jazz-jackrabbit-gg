#!/usr/bin/env bash
# Z80 leg of the differential test: run the GG_TRACE ROM per stage in the
# harness and compare its records with the host GG logic's.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T="$ROOT/build/diff"; FR="${ROMFRAMES:-1500}"
fail=0
for seed in ${SEEDS:-0xACE1 0x1234 0xBEEF 0x0F0F}; do
for st in 0 1 2 3 4 5 6; do
    # the trace ROM waits 10 frames after crt0 has initialised RAM, then
    # reads traceStage / traceFrames
    printf 'symbols %s\nrun 3\npoke traceStage %d w1\npoke traceFrames %d\npoke traceSeed %d\nrun %d\n' \
        "$ROOT/jazz_trace.noi" $st $FR $((seed)) $((FR * 3 + 200)) > "$T/rom_$st.txt"
    SMSTEST_TRACE="$T/rom_$st.bin" "$ROOT/tools/smstest/smstest" "$ROOT/jazz_trace.gg" "$T/rom_$st.txt" > /dev/null
    "$T/diff_gg" "$ROOT/gen/data.bin" $st $FR $seed > "$T/ggh_$st.bin"
    if out=$(python3 "$ROOT/tests/diff/cmp.py" "$T/ggh_$st.bin" "$T/rom_$st.bin"); then
        echo "seed $seed stage $((st+1)) Z80:  $out"
    else
        echo "seed $seed stage $((st+1)) Z80:  $out"; fail=1
    fi
done
done
exit $fail
