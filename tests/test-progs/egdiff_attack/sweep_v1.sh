#!/bin/bash
#
# Step 1 sweep: measurement robustness of the Mode 1 harness.
#
# Builds poc_v1 for several target values and runs it RUNS times each with
# EgDiff enabled, then runs the negative controls once.  Reports, per value,
# how many runs were fully correct and which bytes (if any) were unresolved.
#
# The default value list deliberately includes bytes equal to both
# architectural sentinels (0xA5 and 0x5A) and a zero byte, because those were
# exactly the cases the original single-sentinel estimator could not measure.
#
# Usage: tests/test-progs/egdiff_attack/sweep_v1.sh
#
# Environment:
#   GEM5    gem5 binary     (default build/RISCV/gem5.opt)
#   WORK    output dir      (default /tmp/opencode/egdiff_sweep1)
#   CC      compiler        (default riscv64-linux-gnu-gcc)
#   RUNS    runs per value  (default 2)
#   VALUES  space-separated C 64-bit literals
#   REPS    attempts/byte passed to the harness (default: harness default)

set -u
cd "$(dirname "$0")/../../.." || exit 1

GEM5="${GEM5:-build/RISCV/gem5.opt}"
WORK="${WORK:-/tmp/opencode/egdiff_sweep1}"
CC="${CC:-riscv64-linux-gnu-gcc}"
RUNS="${RUNS:-2}"
REPS="${REPS:-}"
VALUES="${VALUES:-0x1122334455667788ULL 0xDEADBEEFCAFEBABEULL 0xFFFF0000FFFF0000ULL 0x0011223344556677ULL 0xA5A5A5A55A5A5A5AULL 0x5A5A5A5AA5A5A5A5ULL}"
D=tests/test-progs/egdiff_attack
SRC=$D/poc_v1_specgvq_1shot.c

[ -x "$GEM5" ] || { echo "missing gem5: $GEM5"; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"

run_one() { # run_one <bin> <dir> <type> <on>
    local bin="$1" dir="$2" type="$3" on="$4"
    mkdir -p "$dir"
    EGDIFF_TYPE="$type" EGDIFF_ON="$on" timeout 2400 "$GEM5" -d "$dir" \
        configs/example/egdiff.py --cpu-type=O3CPU --caches --cmd="$bin" \
        > "$dir/stdout" 2>&1
    grep -a "poc_v1 " "$dir/stdout" | tail -1
}

printf '%-22s %-6s %-8s %-7s %s\n' "value" "runs" "match" "known" "unknown bytes"
printf '%s\n' "--------------------------------------------------------------------------"

for v in $VALUES; do
    bin="$WORK/poc1_$v"
    extra=""
    [ -n "$REPS" ] && extra="-DREPS=$REPS"
    "$CC" -static -O2 -fno-pic -no-pie $extra -DSECRET_VAL="$v" -o "$bin" \
        "$SRC" "$D/egdiff_secret.c" \
        || { echo "build failed for $v"; continue; }

    declared_match=0
    known_all=0xff
    detail=""
    for r in $(seq 1 "$RUNS"); do
        line=$(run_one "$bin" "$WORK/run_${v}_${r}" egdiff 1)
        case "$line" in *" MATCH") declared_match=$((declared_match+1));; esac
        k=$(printf '%s\n' "$line" | sed -nE 's/.*known=([0-9a-fA-F]+).*/\1/p')
        if [ -n "$k" ]; then
            known_all=$(( known_all & 0x$k ))
        else
            known_all=0
        fi
        # collect any UNKNOWN byte indices from the per-byte trace
        nb=$(grep -a "^byte " "$WORK/run_${v}_${r}/stdout" | grep -a UNKNOWN \
             | sed -nE 's/^byte ([0-9]+):.*/\1/p' | paste -sd, -)
        [ -n "$nb" ] && detail="$detail r$r:[$nb]"
    done

    printf '%-22s %-6s %-8s 0x%02x    %s\n' \
        "$v" "$declared_match/$RUNS" "$declared_match/$RUNS" "$known_all" "${detail:-none}"
done

echo
echo "== negative controls (same binary, default value) =="
bin="$WORK/poc1_ctl"
"$CC" -static -O2 -fno-pic -no-pie -o "$bin" "$SRC" "$D/egdiff_secret.c" || exit 1
printf '  off  : %s\n' "$(run_one "$bin" "$WORK/ctl_off"  egdiff 0)"
printf '  flop : %s\n' "$(run_one "$bin" "$WORK/ctl_flop" flop   1)"

echo
echo "Outputs in $WORK"
