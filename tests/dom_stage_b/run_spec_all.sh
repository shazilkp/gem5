#!/bin/bash
# Run the Stage B arms over the selected SPEC CPU2017 workloads.
#
# usage:
#   run_spec_all.sh [maxinsts]
#
# Environment:
#   SPEC=/path/to/benchspec/CPU
#   RUN=run_base_refrate_riscv.0000   (default: test run dir)
#   PRIMARY_ONLY=1                    (skip out-of-scope 65K/1 MiB arms)
#   DOM_OUT=/results/dir              (default: /tmp/opencode/dom_stage_b)
#
# The list below matches the workloads used by the Stage A characterization.
# Missing run directories are skipped so the same script works locally and on
# the server.
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
SPEC=${SPEC:-$HOME/cpu2017/benchspec/CPU}
MAX=${1:-${MAX:-50000000}}
RUN=${RUN:-run_base_test_riscv.0000}

run_one() {
    local bench=$1 bin=$2 opts=$3
    if [ ! -x "$SPEC/$bench/run/$RUN/$bin" ]; then
        echo "skip $bench (missing $SPEC/$bench/run/$RUN/$bin)"
        return
    fi
    echo "=== $bench ($MAX insts) ==="
    bash "$ROOT/tests/dom_stage_b/run_spec_arms.sh" \
        "$bench" "$RUN" "$bin" "$opts" "$MAX"
}

run_one 520.omnetpp_r omnetpp_r_base.riscv "-c General -r 0"
run_one 505.mcf_r mcf_r_base.riscv "inp.in"
run_one 519.lbm_r lbm_r_base.riscv "20 reference.dat 0 1 100_100_130_cf_a.of"
run_one 523.xalancbmk_r cpuxalan_r_base.riscv "-v test.xml xalanc.xsl"
run_one 541.leela_r leela_r_base.riscv "test.sgf"
run_one 531.deepsjeng_r deepsjeng_r_base.riscv "test.txt"
run_one 548.exchange2_r exchange2_r_base.riscv "0"
