#!/bin/bash
# B5 page-fault/syscall regression with the provider consistency check.
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
OUT=${1:-/tmp/opencode/dom_stage_b}
BIN=$ROOT/tests/test-progs/dom_stage_b/page_fault
GEM5=$ROOT/build/RISCV/gem5.opt
SE=$ROOT/configs/deprecated/example/se.py

riscv64-linux-gnu-gcc -O1 -static -o "$BIN" "$BIN.c"

run_one() {
    local name=$1 envspec=$2
    local dir="$OUT/page_fault_$name"
    rm -rf "$dir"
    mkdir -p "$dir"
    if ! (
        cd "$dir"
        env $envspec timeout "${GEM5_TIMEOUT:-180}" taskset -c 2 "$GEM5" \
            -d "$dir/gem5" "$SE" \
            --cpu-type=O3CPU --caches \
            --cmd="$BIN" \
            --output="$dir/out.txt" \
            --errout="$dir/err.txt" \
            > "$dir/log" 2>&1
    ); then
        echo "FAIL: page_fault/$name did not complete" >&2
        exit 1
    fi
}

run_one base ""
run_one dom "GEM5_DOM=1 GEM5_PROV=1 GEM5_PROV_ENTRIES=65536 GEM5_PROV_CHECK=1"

if ! cmp -s "$OUT/page_fault_base/out.txt" "$OUT/page_fault_dom/out.txt"; then
    echo "FAIL: page_fault output differs" >&2
    exit 1
fi

echo "page_fault: output equal; provider check clean"
