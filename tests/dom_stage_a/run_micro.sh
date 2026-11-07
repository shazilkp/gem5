#!/bin/bash
# Stage A microbenchmarks: array patterns (v0/v1), spec-hit provenance,
# and squash handling.  Run from the gem5 repo root.
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
OUT=${1:-/tmp/opencode/dom_stage_a}
GEM5=$ROOT/build/RISCV/gem5.opt
SE=$ROOT/configs/deprecated/example/se.py
PROGS=$ROOT/tests/test-progs/dom_stage_a

if [ ! -x "$GEM5" ]; then
    echo "missing gem5: $GEM5" >&2
    exit 2
fi

mkdir -p "$OUT"

run_one() {
    local name=$1
    riscv64-linux-gnu-gcc -O1 -static -o "$PROGS/$name" "$PROGS/$name.c"
    rm -rf "$OUT/$name"
    mkdir -p "$OUT/$name"
    (
        cd "$OUT/$name"
        GEM5_DOM_STAGE_A_TRACE="$OUT/$name/trace.txt" \
            taskset -c 2 "$GEM5" -d "$OUT/$name/gem5" "$SE" \
            --cpu-type=O3CPU --caches \
            --cmd="$PROGS/$name" \
            > "$OUT/$name/stdout.txt" 2>&1
    )
    grep -E "Exiting|commitStats0.ipc" "$OUT/$name/gem5/stats.txt" \
        2>/dev/null | tail -2 || true
    ls -lh "$OUT/$name/trace.txt"
}

run_one array_pattern
python3 "$ROOT/tests/dom_stage_a/check_trace.py" \
    "$OUT/array_pattern/trace.txt" --stdout "$OUT/array_pattern/stdout.txt"

run_one spec_hit
python3 "$ROOT/tests/dom_stage_a/check_v1.py" \
    "$OUT/spec_hit/trace.txt" --mode spec

run_one squash
python3 "$ROOT/tests/dom_stage_a/check_v1.py" \
    "$OUT/squash/trace.txt" --mode squash
