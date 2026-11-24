#!/bin/bash
# EgDiff prediction-latency sensitivity for Mode 1.
#   EGDIFF_LATENCY          normal prediction latency (paper: 3 cycles)
#   EGDIFF_DEFERRED_LATENCY deferred prediction latency (paper: +2 cycles)
# Runs the DIVS=1 (minimum working address delay) and DIVS=12 binaries.
set -u
cd "$(dirname "$0")/../../.." || exit 1
GEM5="${GEM5:-build/RISCV/gem5.opt}"
D=tests/test-progs/egdiff_attack
WORK="${WORK:-/tmp/opencode/egdiff_latency_sweep}"
BINDIR="${BINDIR:-/tmp/opencode/egdiff_div_sweep}"

[ -x "$GEM5" ] || { echo "missing gem5: $GEM5"; exit 1; }
mkdir -p "$WORK"

run_one() { # run_one <divs> <lat> <dlat> <tag>
    local divs="$1" lat="$2" dlat="$3" tag="$4"
    local bin="$BINDIR/poc_v1_div$divs"
    [ -x "$bin" ] || { echo "missing $bin"; return; }
    local out="$WORK/${tag}_d${divs}_l${lat}_dl${dlat}"
    rm -rf "$out"; mkdir -p "$out"
    EGDIFF_TYPE=egdiff EGDIFF_ON=1 \
    EGDIFF_LATENCY="$lat" EGDIFF_DEFERRED_LATENCY="$dlat" \
    timeout 900 "$GEM5" -d "$out" \
        configs/example/egdiff.py --cpu-type=O3CPU --caches \
        --cmd="$bin" > "$out/stdout" 2>&1
    local line
    line=$(grep -a "poc_v1 secret=" "$out/stdout" | tail -1)
    echo "DIVS=$divs LAT=$lat DEF=$dlat ${line:-NO-OUTPUT}"
}

for d in 1 12; do
    run_one "$d" 0 0 baseline
    run_one "$d" 3 2 paper
    run_one "$d" 5 2 high
done

# Off control at the paper latency (DIVS=1 binary).
out="$WORK/off_d1_l3_dl2"
rm -rf "$out"; mkdir -p "$out"
EGDIFF_TYPE=egdiff EGDIFF_ON=0 EGDIFF_LATENCY=3 EGDIFF_DEFERRED_LATENCY=2 \
timeout 900 "$GEM5" -d "$out" configs/example/egdiff.py --cpu-type=O3CPU \
    --caches --cmd="$BINDIR/poc_v1_div1" > "$out/stdout" 2>&1
line=$(grep -a "poc_v1 secret=" "$out/stdout" | tail -1)
echo "DIVS=1 OFF LAT=3 DEF=2 ${line:-NO-OUTPUT}"
