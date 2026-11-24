#!/bin/bash
# Sweep the register-only address-delay length in the Mode 1 PoC.
# The prediction is injected at dispatch; the delay only postpones the
# mispredicting load's data access/verification.  The sweep reports the
# minimum delay at which the attack still recovers.
set -u
cd "$(dirname "$0")/../../.." || exit 1
GEM5="${GEM5:-build/RISCV/gem5.opt}"
CC="${CC:-riscv64-linux-gnu-gcc}"
WORK="${WORK:-/tmp/opencode/egdiff_div_sweep}"
D=tests/test-progs/egdiff_attack

[ -x "$GEM5" ] || { echo "missing gem5: $GEM5"; exit 1; }
mkdir -p "$WORK"

for d in 0 1 2 3 4 6 8 10 12; do
    bin="$WORK/poc_v1_div$d"
    "$CC" -static -O2 -fno-pic -no-pie -DATK_DIVS="$d" \
        -o "$bin" "$D/poc_v1_specgvq_1shot.c" "$D/egdiff_secret.c" || exit 1
    out="$WORK/on$d"
    rm -rf "$out"; mkdir -p "$out"
    EGDIFF_TYPE=egdiff EGDIFF_ON=1 timeout 900 "$GEM5" -d "$out" \
        configs/example/egdiff.py --cpu-type=O3CPU --caches \
        --cmd="$bin" > "$out/stdout" 2>&1
    line=$(grep -a "poc_v1 secret=" "$out/stdout" | tail -1)
    echo "DIVS=$d ON  ${line:-NO-OUTPUT}"
done

for d in 0 12; do
    bin="$WORK/poc_v1_div$d"
    out="$WORK/off$d"
    rm -rf "$out"; mkdir -p "$out"
    EGDIFF_TYPE=egdiff EGDIFF_ON=0 timeout 900 "$GEM5" -d "$out" \
        configs/example/egdiff.py --cpu-type=O3CPU --caches \
        --cmd="$bin" > "$out/stdout" 2>&1
    line=$(grep -a "poc_v1 secret=" "$out/stdout" | tail -1)
    echo "DIVS=$d OFF ${line:-NO-OUTPUT}"
done
