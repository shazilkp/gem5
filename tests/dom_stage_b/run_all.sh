#!/bin/bash
# Run the complete Stage B micro test suite.  Every gem5 invocation has a
# hard timeout, so a shadow-buffer deadlock is reported as a failure instead
# of hanging the shell.
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
OUT=${1:-/tmp/opencode/dom_stage_b}

bash "$ROOT/tests/dom_stage_b/run_unit.sh" "$OUT"
bash "$ROOT/tests/dom_stage_b/run_b0.sh" "$OUT"
bash "$ROOT/tests/dom_stage_b/run_b2.sh" "$OUT"
bash "$ROOT/tests/dom_stage_b/run_e.sh" "$OUT"
bash "$ROOT/tests/dom_stage_b/run_correctness.sh" "$OUT"
bash "$ROOT/tests/dom_stage_b/run_provider.sh" "$OUT"
bash "$ROOT/tests/dom_stage_b/run_pagefault.sh" "$OUT"
bash "$ROOT/tests/dom_stage_b/run_stress.sh" "$OUT"

echo "all Stage B micro tests passed"
