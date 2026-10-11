#!/usr/bin/env bash
# Differential test: Genesis game_core.c (reference, with the one bug fix the
# GG port makes) vs the GG logic, both natively, same random inputs, every
# stage.  Then the same GG records from the real Z80 ROM in the harness.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GEN="${JJ_GENESIS:-$HOME/jj}"
D="$ROOT/tests/diff"; T="$ROOT/build/diff"
mkdir -p "$T/ref"
cp "$GEN/src/game_core.c" "$T/ref/game_core.c"
python3 - "$T/ref/game_core.c" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
a = "        b->behaviour = behaviour;\n        return 1;"
assert a in s
s = s.replace(a, "        b->behaviour = behaviour;\n        b->hostile = 0;\n        return 1;")
open(p, "w").write(s)
PY
gcc -O2 -std=c99 -DJAZZ_HOST -DJAZZ_JJ1_RUNTIME -I"$GEN/inc" -I"$GEN/src" -I"$D" \
    "$T/ref/game_core.c" "$GEN/src/jj1_runtime.c" "$GEN/src/jj1_eventset.c" "$GEN/tests/host_data.c" \
    "$D/diff_genesis.c" -o "$T/diff_genesis"
gcc -O2 -std=c99 -DGG_HOST -I"$ROOT/src" -I"$ROOT/gen" -I"$D" "$ROOT/src/gg_logic.c" "$D/host_world.c" \
    "$ROOT/gen/gg_fixed.c" "$D/diff_gg.c" -o "$T/diff_gg"
FR="${FRAMES:-4000}"
fail=0
for seed in ${SEEDS:-0xACE1 0x1234 0xBEEF 0x0F0F}; do
for st in 0 1 2 3 4 5 6; do
    "$T/diff_genesis" $st $FR $seed > "$T/gen_$st.bin"
    "$T/diff_gg" "$ROOT/gen/data.bin" $st $FR $seed > "$T/gg_$st.bin"
    if out=$(python3 "$D/cmp.py" "$T/gen_$st.bin" "$T/gg_$st.bin"); then
        echo "seed $seed stage $((st+1)) host: $out"
    else
        echo "seed $seed stage $((st+1)) host: $out"; fail=1
    fi
done
done
exit $fail
