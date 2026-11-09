#!/bin/bash
# B2: shadowed L1 hit with deferred replacement update.
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
OUT=${1:-/tmp/opencode/dom_stage_b}
BIN=$ROOT/tests/test-progs/dom_stage_b/shadow_hit
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
        "$@" timeout "${GEM5_TIMEOUT:-180}" taskset -c 2 "$GEM5" -d "$OUT/$name/gem5" "$SE" \
            --cpu-type=O3CPU --caches \
            --cmd="$BIN" \
            --output="$OUT/$name/out.txt" \
            --errout="$OUT/$name/err.txt" \
            > "$OUT/$name/log" 2>&1
    )
}

run_one shadow_hit_base env
run_one shadow_hit_dom env GEM5_DOM=1

python3 "$ROOT/tests/dom_stage_b/check_b2.py" \
    "$OUT/shadow_hit_base" "$OUT/shadow_hit_dom"
