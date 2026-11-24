#!/bin/bash
# Run the branch-squash control under gem5 with the LVP disabled.
set -u
cd "$(dirname "$0")/../../.." || exit 1
GEM5="${GEM5:-build/RISCV/gem5.opt}"
CC="${CC:-riscv64-linux-gnu-gcc}"
WORK="${WORK:-/tmp/opencode/branch_squash}"
D=tests/test-progs/egdiff_attack
BIN="$WORK/branch_squash_probe"

[ -x "$GEM5" ] || { echo "missing gem5: $GEM5"; exit 1; }
mkdir -p "$WORK"

"$CC" -static -O2 -fno-pic -no-pie -fno-builtin -o "$BIN" \
    "$D/branch_squash_probe.c" || exit 1

for mode in arch squash none; do
    out="$WORK/$mode"
    rm -rf "$out"; mkdir -p "$out"
    EGDIFF_ON=0 "$GEM5" -d "$out" \
        configs/example/egdiff.py --cpu-type=O3CPU --caches \
        --cmd="$BIN" --options="$mode" > "$out/stdout" 2>&1
    echo "== $mode"
    grep -a "mode=" "$out/stdout" || tail -5 "$out/stdout"
    grep -a "branchMispredicts\|branchMisses\|numMispred" "$out/stats.txt" 2>/dev/null | head -5
done
