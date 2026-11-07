#!/bin/bash
# B0: build the branch-shadow microbenchmark and run it with and without DoM.
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
OUT=${1:-/tmp/opencode/dom_stage_b}
BIN=$ROOT/tests/test-progs/dom_stage_b/branch_shadow
GEM5=$ROOT/build/RISCV/gem5.opt
SE=$ROOT/configs/deprecated/example/se.py

riscv64-linux-gnu-gcc -O1 -static -o "$BIN" "$BIN.c"

run_one() {
    local name=$1
    shift
    rm -rf "$OUT/$name"
    mkdir -p "$OUT/$name"
    (
        cd "$OUT/$name"
        "$@" taskset -c 2 "$GEM5" -d "$OUT/$name/gem5" "$SE" \
            --cpu-type=O3CPU --caches \
            --cmd="$BIN" \
            --output="$OUT/$name/out.txt" \
            --errout="$OUT/$name/err.txt" \
            > "$OUT/$name/log" 2>&1
    )
}

run_one base env
run_one dom env GEM5_DOM=1

python3 "$ROOT/tests/dom_stage_b/check_b0.py" "$OUT/base" "$OUT/dom"
