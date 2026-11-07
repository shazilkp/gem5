#!/bin/bash
# Host-side B1 unit test (no gem5 build required).
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
OUT=${1:-/tmp/opencode/dom_stage_b}
mkdir -p "$OUT"

g++ -std=c++17 -Wall -Wextra -I "$ROOT/src" \
    -o "$OUT/test_shadow_buffer" \
    "$ROOT/tests/dom_stage_b/test_shadow_buffer.cc"

"$OUT/test_shadow_buffer"
