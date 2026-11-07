#!/bin/bash
# Run one SPEC benchmark through the Stage A probe, then replay the trace
# through the per-word and region oracles.
#
# usage:
#   run_spec.sh <bench_dir> <run_dir> <binary> <options> [maxinsts] [core]
#
# example (mcf test input):
#   tests/dom_stage_a/run_spec.sh 505.mcf_r run_base_test_riscv.0000 \
#       ./mcf_r_base.riscv inp.in 50000000 2
set -euo pipefail

if [ $# -lt 4 ]; then
    echo "usage: $0 <bench_dir> <run_dir> <binary> <options> [maxinsts] [core]" >&2
    exit 2
fi

ROOT=$(git rev-parse --show-toplevel)
SPEC=${SPEC:-$HOME/cpu2017/benchspec/CPU}
BENCH=$1
RUN=$2
BIN=$3
OPTS=$4
MAX=${5:-50000000}
CORE=${6:-2}

RUNDIR="$SPEC/$BENCH/run/$RUN"
OUT=${DOM_OUT:-/tmp/dom}/$BENCH

if [ ! -x "$RUNDIR/$BIN" ]; then
    echo "missing binary: $RUNDIR/$BIN" >&2
    exit 2
fi
if [ ! -x "$ROOT/build/RISCV/gem5.opt" ]; then
    echo "missing gem5: $ROOT/build/RISCV/gem5.opt (build it first)" >&2
    exit 2
fi

rm -rf "$OUT"
mkdir -p "$OUT"

echo "bench=$BENCH run=$RUN maxinsts=$MAX core=$CORE"
echo "out=$OUT"

(
    cd "$RUNDIR"
    GEM5_DOM_STAGE_A_TRACE="$OUT/trace.txt" \
        taskset -c "$CORE" "$ROOT/build/RISCV/gem5.opt" -d "$OUT/gem5" \
        "$ROOT/configs/deprecated/example/se.py" \
        --cpu-type=O3CPU --caches \
        --cmd="$BIN" --options="$OPTS" \
        --output="$OUT/program.out" --errout="$OUT/program.err" \
        --maxinsts="$MAX"
) > "$OUT/gem5.log" 2>&1

grep -E "Exiting|numCycles |commitStats0.ipc " "$OUT/gem5.log" "$OUT/gem5/stats.txt" 2>/dev/null | tail -3 || true
ls -lh "$OUT/trace.txt"
echo "--- per-word (T2) ---"
python3 "$ROOT/tools/dom_stage_a/profile_trace.py" "$OUT/trace.txt" --capacity 4096
python3 "$ROOT/tools/dom_stage_a/profile_trace.py" "$OUT/trace.txt" --capacity 65536
echo "--- region (T3) ---"
python3 "$ROOT/tools/dom_stage_a/profile_regions.py" "$OUT/trace.txt" --block-size 64
python3 "$ROOT/tools/dom_stage_a/profile_regions.py" "$OUT/trace.txt" --block-size 4096
