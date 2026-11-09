#!/bin/bash
# Stage B SPEC arm runner.
#
# usage:
#   run_spec_arms.sh <bench> <run> <binary> <options> [maxinsts] [core]
#
# example:
#   run_spec_arms.sh 505.mcf_r run_base_test_riscv.0000 \
#       mcf_r_base.riscv "inp.in" 50000000 2
#
# Arms:
#   base           64 KiB L1, no mitigation
#   dom            DoM, 64 KiB L1
#   dom_prov4k     DoM + 4K-word provider (~64 KiB)
#   dom_prov65k    DoM + 65K-word provider (~1 MiB)
#   dom_l1_128k    DoM, 128 KiB L1 (equal area to the 4K provider)
#   dom_l1_1m      DoM, 1 MiB L1 (equal area to the 65K provider)
#   base_l1_1m     1 MiB L1, no mitigation
set -euo pipefail

if [ $# -lt 4 ]; then
    echo "usage: $0 <bench> <run> <binary> <options> [maxinsts] [core]" >&2
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
OUT=${DOM_OUT:-/tmp/opencode/dom_stage_b}/spec/$BENCH
GEM5=$ROOT/build/RISCV/gem5.opt
SE=$ROOT/configs/deprecated/example/se.py

if [ ! -x "$RUNDIR/$BIN" ]; then
    echo "missing SPEC binary: $RUNDIR/$BIN" >&2
    exit 2
fi
if [ ! -x "$GEM5" ]; then
    echo "missing gem5: $GEM5" >&2
    exit 2
fi

mkdir -p "$OUT"

run_arm() {
    local arm=$1 envspec=$2 extra=$3
    local dir="$OUT/$arm"
    rm -rf "$dir"
    mkdir -p "$dir"

    (
        cd "$RUNDIR"
        env $envspec timeout "${GEM5_TIMEOUT:-0}" taskset -c "$CORE" "$GEM5" -d "$dir/gem5" "$SE" \
            --cpu-type=O3CPU --caches $extra \
            --cmd="$BIN" --options="$OPTS" \
            --output="$dir/program.out" --errout="$dir/program.err" \
            --maxinsts="$MAX"
    ) > "$dir/gem5.log" 2>&1

    python3 - "$BENCH" "$arm" "$dir/gem5/stats.txt" "$OUT/ipc.tsv" <<'PY'
import sys

bench, arm, stats_path, out_path = sys.argv[1:5]
values = {}
try:
    with open(stats_path, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue
            key = parts[0]
            if key in ("system.cpu.commitStats0.ipc",
                       "system.cpu.commitStats0.numInsts",
                       "system.cpu.numCycles",
                       "system.cpu.dcache.demandMisses",
                       "system.cpu.dcache.demandHits"):
                values[key] = float(parts[1])
            if key.endswith(".domProvHits"):
                values["domProvHits"] = values.get("domProvHits", 0.0) + float(parts[1])
            if key.endswith(".domDelayedMisses"):
                values["domDelayedMisses"] = values.get("domDelayedMisses", 0.0) + float(parts[1])
            if key.endswith(".domHitsDeferred"):
                values["domHitsDeferred"] = values.get("domHitsDeferred", 0.0) + float(parts[1])
except FileNotFoundError:
    pass

row = [
    bench, arm,
    f"{values.get('system.cpu.commitStats0.ipc', float('nan')):.6f}",
    f"{values.get('system.cpu.commitStats0.numInsts', 0):.0f}",
    f"{values.get('system.cpu.numCycles', 0):.0f}",
    f"{values.get('system.cpu.dcache.demandMisses', 0):.0f}",
    f"{values.get('domProvHits', 0):.0f}",
    f"{values.get('domDelayedMisses', 0):.0f}",
    f"{values.get('domHitsDeferred', 0):.0f}",
]
with open(out_path, "a", encoding="utf-8") as f:
    f.write("\t".join(row) + "\n")
print("\t".join(row))
PY
}

printf "bench\tarm\tipc\tinsts\tcycles\tdcacheMisses\tprovHits\tdomDelayed\tdomHitDeferred\n"
run_arm base "" ""
run_arm dom "GEM5_DOM=1" ""
run_arm dom_prov4k "GEM5_DOM=1 GEM5_PROV=1 GEM5_PROV_ENTRIES=4096" ""
run_arm dom_prov65k "GEM5_DOM=1 GEM5_PROV=1 GEM5_PROV_ENTRIES=65536" ""
run_arm dom_l1_128k "GEM5_DOM=1" "--l1d_size=128KiB"
run_arm dom_l1_1m "GEM5_DOM=1" "--l1d_size=1MiB"
run_arm base_l1_1m "" "--l1d_size=1MiB"
