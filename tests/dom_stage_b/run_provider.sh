#!/bin/bash
# B5: DoM + exact committed-value provider vs baseline.
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
        env $envspec timeout "${GEM5_TIMEOUT:-180}" taskset -c 2 "$GEM5" -d "$dir/gem5" "$SE" \
            --cpu-type=O3CPU --caches \
            --cmd="$PROGS/$prog" \
            --output="$dir/out.txt" \
            --errout="$dir/err.txt" \
            > "$dir/log" 2>&1
    )
}

for prog in provider_hit provider_update; do
    riscv64-linux-gnu-gcc -O1 -static -o "$PROGS/$prog" "$PROGS/$prog.c"
    run_one "$prog" base ""
    run_one "$prog" dom "GEM5_DOM=1 GEM5_PROV=1 GEM5_PROV_ENTRIES=65536 GEM5_PROV_CHECK=1"
    python3 "$ROOT/tests/dom_stage_b/check_provider.py" \
        "$OUT/${prog}_base" "$OUT/${prog}_dom"
done
