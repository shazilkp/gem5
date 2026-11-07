#!/bin/bash
# Stage A smoke run: build one microbenchmark, run it under O3 + caches,
# and check the trace.  Intended for local preliminary validation.
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
BIN=$ROOT/tests/test-progs/dom_stage_a/array_pattern
OUT=${1:-/tmp/opencode/dom_stage_a}

riscv64-linux-gnu-gcc -O1 -static -o "$BIN" "$BIN.c"

rm -rf "$OUT"
mkdir -p "$OUT"

GEM5_DOM_STAGE_A_TRACE="$OUT/trace.txt" \
    taskset -c 2 "$ROOT/build/RISCV/gem5.opt" -d "$OUT/gem5" \
    "$ROOT/configs/deprecated/example/se.py" \
    --cpu-type=O3CPU --caches --cmd="$BIN" \
    > "$OUT/stdout.txt" 2>&1

python3 "$ROOT/tests/dom_stage_a/check_trace.py" \
    "$OUT/trace.txt" --stdout "$OUT/stdout.txt"
