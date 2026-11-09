#!/bin/bash
# Liveness/stress tests for Delay-on-Miss: serial miss chain and all-hit loop.
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
OUT=${1:-/tmp/opencode/dom_stage_b}
PROGS=$ROOT/tests/test-progs/dom_stage_b
GEM5=$ROOT/build/RISCV/gem5.opt
SE=$ROOT/configs/deprecated/example/se.py
GEM5_TIMEOUT=${GEM5_TIMEOUT:-180}

run_one() {
    local prog=$1 name=$2 envspec=$3
    local dir="$OUT/${prog}_${name}"
    rm -rf "$dir"
    mkdir -p "$dir"

    if ! (
        cd "$dir"
        env $envspec timeout "$GEM5_TIMEOUT" taskset -c 2 "$GEM5" \
            -d "$dir/gem5" "$SE" \
            --cpu-type=O3CPU --caches \
            --cmd="$PROGS/$prog" \
            --output="$dir/out.txt" \
            --errout="$dir/err.txt" \
            > "$dir/log" 2>&1
    ); then
        echo "FAIL: $prog/$name did not complete (hang/timeout/error)" >&2
        exit 1
    fi
}

for prog in load_chain hit_loop; do
    riscv64-linux-gnu-gcc -O1 -static -o "$PROGS/$prog" "$PROGS/$prog.c"
    run_one "$prog" base ""
    run_one "$prog" dom "GEM5_DOM=1"
done

python3 "$ROOT/tests/dom_stage_b/check_stress.py" "$OUT"
