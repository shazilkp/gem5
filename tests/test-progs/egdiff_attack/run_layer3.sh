#!/bin/bash
#
# Layer 3 semantic verification: deferred prediction (Section III-C).
#
#   A) both loads miss        -> egdiffDeferredBindings > 0, DeferredPredictions > 0
#   B) dependent hits early   -> egdiffDeferredIgnored > 0
#   C) base completed earlier -> egdiffPredictions > 0 (normal dispatch path)
#   all phases: exact checksum, numPredictionIncorrect == 0
#
# Usage: tests/test-progs/egdiff_attack/run_layer3.sh
#
# Environment:
#   GEM5    gem5 binary        (default build/RISCV/gem5.opt)
#   WORK    output directory   (default /tmp/opencode/egdiff_layer3)

set -u
cd "$(dirname "$0")/../../.." || exit 1

GEM5="${GEM5:-build/RISCV/gem5.opt}"
WORK="${WORK:-/tmp/opencode/egdiff_layer3}"
CC="${CC:-riscv64-linux-gnu-gcc}"
D=tests/test-progs/egdiff_attack
BIN=$D/test_layer3_deferred_wake

[ -x "$GEM5" ] || { echo "missing gem5: $GEM5"; exit 1; }

pass=0; fail=0
ok()  { echo "  PASS  $1"; pass=$((pass+1)); }
bad() { echo "  FAIL  $1"; fail=$((fail+1)); }

echo "== build =="
"$CC" -static -O1 -o "$BIN" "$D/test_layer3_deferred_wake.c" || exit 1

rm -rf "$WORK"; mkdir -p "$WORK"

echo "== run =="
EGDIFF_TYPE=egdiff EGDIFF_ON=1 \
timeout 1800 "$GEM5" -d "$WORK" --debug-flags=LVP,EgDiff \
    --debug-file="$WORK/simout" \
    configs/example/egdiff.py --cpu-type=O3CPU --caches \
    --cmd="$BIN" > "$WORK/stdout" 2>&1
rc=$?
[ $rc -eq 0 ] || { echo "gem5 failed rc=$rc"; tail -30 "$WORK/stdout"; exit 1; }

TR="$WORK/simout"; ST="$WORK/stats.txt"

echo "== architectural correctness =="
RUNOUT=$(grep -a "layer3 sink=" "$WORK/stdout" | tail -1)
case "$RUNOUT" in
  *" OK") ok "architectural checksum exact" ;;
  *)      bad "architectural checksum wrong: '$RUNOUT'" ;;
esac

stat() { awk -v k="$1" '$1 ~ ("loadValuePredictor\\." k "$") {print $2}' "$ST"; }
gt0()  { local v; v=$(stat "$1"); [ "${v:-0}" -gt 0 ] && ok "$1 = $v (> 0)" \
                                                  || bad "$1 = ${v:-missing} (expected > 0)"; }
eq()   { local v; v=$(stat "$1"); [ "${v:-x}" = "$2" ] && ok "$1 = $v" \
                                                       || bad "$1 = ${v:-missing} (expected $2)"; }

echo "== semantic checks =="
gt0 egdiffDeferredBindings
gt0 egdiffDeferredPredictions
gt0 egdiffDeferredIgnored
gt0 egdiffPredictions
gt0 egdiffNVQBases
gt0 numPredictionCorrect
eq  numPredictionIncorrect 0

# Phase attribution by trained distance (robust to code-layout changes):
#   phase A  publishes with d = 2  (both loads stream, base unresolved)
#   phase C  publishes with d = 22 (base already completed at dispatch)
if grep -aq "deferred wakeup -> publish .* (d=2)$" "$TR"; then
    ok "phase A: deferred wakeup delivered at d=2"
else
    bad "phase A: no deferred wakeup delivered at d=2"
fi
if grep -aq "deferred wakeup -> publish .* (d=22)$" "$TR"; then
    ok "phase C: normal/deferred value delivered at d=22"
else
    bad "phase C: no value delivered at d=22"
fi
if grep -aq "deferred wakeup ignored" "$TR"; then
    ok "phase B: early-completion wakeup ignored"
else
    bad "phase B: no ignored early-completion wakeup"
fi

# Negative control: with the predictor off, the architecture must still be
# exact and every EgDiff statistic must be zero.
NEG=$WORK/neg_off
rm -rf "$NEG"; mkdir -p "$NEG"
EGDIFF_TYPE=egdiff EGDIFF_ON=0 timeout 1800 "$GEM5" -d "$NEG" \
    configs/example/egdiff.py --cpu-type=O3CPU --caches \
    --cmd="$BIN" > "$NEG/stdout" 2>&1
NEGOUT=$(grep -a "layer3 sink=" "$NEG/stdout" | tail -1)
case "$NEGOUT" in
  *" OK") ok "negative control (EgDiff off): checksum exact" ;;
  *)      bad "negative control (EgDiff off): '$NEGOUT'" ;;
esac
nz=0
for k in egdiffCommits egdiffPredictions egdiffDeferredBindings \
         egdiffDeferredPredictions egdiffDeferredIgnored egdiffSquashed; do
    v=$(awk -v k="$k" '$1 ~ ("loadValuePredictor\\." k "$") {print $2}' \
        "$NEG/stats.txt")
    [ "${v:-0}" = "0" ] || nz=$((nz+1))
done
[ $nz -eq 0 ] && ok "negative control: all EgDiff stats are zero" \
              || bad "negative control: $nz EgDiff stat(s) non-zero"

echo
echo "== summary =="
stat egdiffPredictions        | xargs -I{} echo "  normal dispatch predictions : {}"
stat egdiffDeferredBindings   | xargs -I{} echo "  deferred bindings           : {}"
stat egdiffDeferredPredictions| xargs -I{} echo "  deferred wakeups fired      : {}"
stat egdiffDeferredIgnored    | xargs -I{} echo "  deferred wakeups ignored    : {}"
stat numPredictionCorrect     | xargs -I{} echo "  verified-correct            : {}"
stat numPredictionIncorrect   | xargs -I{} echo "  mispredictions              : {}"

echo
echo "== result: $pass passed, $fail failed =="
[ $fail -eq 0 ] || { echo "LAYER 3 FAILED"; exit 1; }
echo "LAYER 3 PASSED"
