#!/bin/bash
# Safe proxy for predictor latency: delay the dependent probe's issue by N
# real instructions (N cycles), instead of scheduling a gem5 event.
set -u
cd "$(dirname "$0")/../../.." || exit 1
GEM5="${GEM5:-build/RISCV/gem5.opt}"
CC="${CC:-riscv64-linux-gnu-gcc}"
WORK="${WORK:-/tmp/opencode/egdiff_probe_delay}"
D=tests/test-progs/egdiff_attack
mkdir -p "$WORK"

run_one() { # run_one <divs> <pd> <dlat> <tag>
    local divs="$1" pd="$2" dlat="$3" tag="$4"
    local bin="$WORK/poc_d${divs}_pd${pd}"
    "$CC" -static -O2 -fno-pic -no-pie -DATK_DIVS="$divs" \
        -DPROBE_DELAY="$pd" -o "$bin" \
        "$D/poc_v1_specgvq_1shot.c" "$D/egdiff_secret.c" || return
    local out="$WORK/${tag}_d${divs}_pd${pd}_dl${dlat}"
    rm -rf "$out"; mkdir -p "$out"
    EGDIFF_TYPE=egdiff EGDIFF_ON=1 EGDIFF_LATENCY=0 \
    EGDIFF_DEFERRED_LATENCY="$dlat" timeout 900 "$GEM5" -d "$out" \
        configs/example/egdiff.py --cpu-type=O3CPU --caches \
        --cmd="$bin" > "$out/stdout" 2>&1
    local line
    line=$(grep -a "poc_v1 secret=" "$out/stdout" | tail -1)
    echo "DIVS=$divs PD=$pd DEF=$dlat ${line:-NO-OUTPUT}"
}

for pd in 0 1 3 5; do
    run_one 12 "$pd" 0 proxy
done
run_one 1 0 0 proxy
run_one 1 3 0 proxy
run_one 12 0 2 deferred
run_one 12 3 2 both
