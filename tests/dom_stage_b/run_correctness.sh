#!/bin/bash
# B1 architectural-equality regression: run several kernels with and without
# Delay-on-Miss and require byte-identical outputs.
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
OUT=${1:-/tmp/opencode/dom_stage_b}
GEM5=$ROOT/build/RISCV/gem5.opt
SE=$ROOT/configs/deprecated/example/se.py
PROGS=$ROOT/tests/test-progs/dom_stage_b

run_one() {
    local prog=$1 name=$2 envspec=$3
    local dir="$OUT/${prog}_${name}"
    rm -rf "$dir"
    mkdir -p "$dir"
    (
        cd "$dir"
        env $envspec taskset -c 2 "$GEM5" -d "$dir/gem5" "$SE" \
            --cpu-type=O3CPU --caches \
            --cmd="$PROGS/$prog" \
            --output="$dir/out.txt" \
            --errout="$dir/err.txt" \
            > "$dir/log" 2>&1
    )
}

for prog in array_branch squash_shadow; do
    riscv64-linux-gnu-gcc -O1 -static -o "$PROGS/$prog" "$PROGS/$prog.c"
    run_one "$prog" base ""
    run_one "$prog" dom "GEM5_DOM=1"
done

python3 "$ROOT/tests/dom_stage_b/check_equal.py" \
    "$OUT/array_branch_base" "$OUT/array_branch_dom" \
    "$OUT/squash_shadow_base" "$OUT/squash_shadow_dom"
