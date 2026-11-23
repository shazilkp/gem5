#!/bin/bash
#
# Layer 1 semantic verification: commit-stage Distance Polling (Algorithm 1).
#
# Builds tests/test-progs/egdiff_attack/test_layer1_polling.c, runs it under
# gem5 with the EgDiff predictor selected and --debug-flags=EgDiff, then
# asserts the exact arithmetic of Algorithm 1 from the trace + stats.
#
# Usage: tests/test-progs/egdiff_attack/run_layer1.sh
#
# Environment:
#   GEM5    gem5 binary        (default build/RISCV/gem5.opt)
#   WORK    output directory   (default /tmp/opencode/egdiff_layer1)

set -u
cd "$(dirname "$0")/../../.." || exit 1

GEM5="${GEM5:-build/RISCV/gem5.opt}"
WORK="${WORK:-/tmp/opencode/egdiff_layer1}"
CC="${CC:-riscv64-linux-gnu-gcc}"
SRC=tests/test-progs/egdiff_attack/test_layer1_polling.c
BIN=tests/test-progs/egdiff_attack/test_layer1_polling

[ -x "$GEM5" ] || { echo "missing gem5: $GEM5"; exit 1; }

echo "== build =="
"$CC" -static -O1 -o "$BIN" "$SRC" || exit 1

rm -rf "$WORK"; mkdir -p "$WORK"

echo "== run =="
EGDIFF_TYPE=egdiff EGDIFF_ON=1 \
timeout 1200 "$GEM5" -d "$WORK" --debug-flags=EgDiff \
    --debug-file="$WORK/simout" \
    configs/example/egdiff.py --cpu-type=O3CPU --caches \
    --cmd="$BIN" > "$WORK/stdout" 2>&1
rc=$?
[ $rc -eq 0 ] || { echo "gem5 failed rc=$rc (see $WORK)"; tail -20 "$WORK/stdout"; exit 1; }

TRACE="$WORK/simout"
STATS=$(ls "$WORK"/stats.txt 2>/dev/null || true)

pass=0; fail=0
check() { # check <name> <pattern>
    if grep -aqE "$2" "$TRACE"; then
        echo "  PASS  $1"; pass=$((pass+1))
    else
        echo "  FAIL  $1   (/$(echo "$2" | head -c 90)/)"; fail=$((fail+1))
    fi
}
checkstat() { # checkstat <name> <regex>
    if [ -n "$STATS" ] && grep -aqE "$2" "$STATS"; then
        echo "  PASS  $1"; pass=$((pass+1))
    else
        echo "  FAIL  $1   (stats: /$2/)"; fail=$((fail+1))
    fi
}

echo "== semantic checks =="

# (1) L_target's entry is allocated at distance 32.
check "entry allocated with distance=32" \
      "alloc=1 .*d 0->32|alloc=1.*d [0-9]+->32"

# (2) Distance polls down one step per mismatching commit.
check "polling step 32->31" "poll=1 .*d 32->31"
check "polling step 31->30" "poll=1 .*d 31->30"
check "polling step 10->9"  "poll=1 .*d 10->9"

# (3) The stride 40 is latched at distance 2 (0x28).
check "diff latched to 40 at d=2" "d [0-9]+->2 diff [0-9a-fx]+->0x28"

# (4) Confidence saturates at the steady distance.
check "stride match at d=2 with diff=40" "stride=1 .*d 2->2 diff 0x28->0x28"

# (5) FPC reaches MAXCONF and usefulness reaches MAXU.
check "confidence counter saturates (ctr=7)" "ctr=7"
check "usefulness counter saturates (u=3)"   "u=3"

# (6) One later mismatch resets confidence, polls 2 -> 1, wraps 1 -> 32,
#     and re-locks onto the new stride 99 (0x63) at d = 2.
check "reset: ctr/u cleared on stride change" "poll=1 .*ctr=0 u=0"
check "reset: distance polls 2->1" "poll=1 .*d 2->1"
check "wrap: distance wraps 1->32" "poll=1 .*d 1->32"
check "re-lock: new stride 99 (0x63) latched at d=2" "d 3->2 diff [0-9a-fx]+->0x63"

# (7) Pipeline statistics agree.
checkstat "egdiffCommits > 0"      "loadValuePredictor\.egdiffCommits +[1-9]"
checkstat "egdiffPollingSteps > 0" "loadValuePredictor\.egdiffPollingSteps +[1-9]"
checkstat "egdiffStrideMatches > 0" "loadValuePredictor\.egdiffStrideMatches +[1-9]"
checkstat "egdiffTableAllocations > 0" "loadValuePredictor\.egdiffTableAllocations +[1-9]"
checkstat "egdiffTickDecays > 0"   "loadValuePredictor\.egdiffTickDecays +[1-9]"

echo
echo "== result: $pass passed, $fail failed =="
[ $fail -eq 0 ] || { echo "LAYER 1 FAILED"; exit 1; }
echo "LAYER 1 PASSED"
