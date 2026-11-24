#!/bin/bash
#
# Layer 2 semantic verification: speculative GVQ lifetime, dispatch prediction,
# LQ verification/squash, the 1024-instruction lastMisp blackout, and ROB-squash
# rollback.
#
# Two binaries:
#   test_layer2_spec_and_squash  clean stride loop + one induced misprediction
#   test_layer2_branch_squash    wrong-path loads discarded by ROB::squash
#
# Usage: tests/test-progs/egdiff_attack/run_layer2.sh
#
# Environment:
#   GEM5    gem5 binary        (default build/RISCV/gem5.opt)
#   WORK    output directory   (default /tmp/opencode/egdiff_layer2)

set -u
cd "$(dirname "$0")/../../.." || exit 1

GEM5="${GEM5:-build/RISCV/gem5.opt}"
WORK="${WORK:-/tmp/opencode/egdiff_layer2}"
CC="${CC:-riscv64-linux-gnu-gcc}"
D=tests/test-progs/egdiff_attack

[ -x "$GEM5" ] || { echo "missing gem5: $GEM5"; exit 1; }

pass=0; fail=0
ok()  { echo "  PASS  $1"; pass=$((pass+1)); }
bad() { echo "  FAIL  $1"; fail=$((fail+1)); }
ct()  { if grep -aqE "$2" "$3"; then ok "$1"; else bad "$1 (/$2/)"; fi; }
cs()  { if grep -aqE "$2" "$3"; then ok "$1"; else bad "$1 (stats /$2/)"; fi; }

run() { # run <name> <src>
    local name="$1" src="$2"
    echo "== build+run $name =="
    "$CC" -static -O1 -o "$D/$name" "$D/$src" || exit 1
    local out="$WORK/$name"
    rm -rf "$out"; mkdir -p "$out"
    EGDIFF_TYPE=egdiff EGDIFF_ON=1 \
    timeout 1800 "$GEM5" -d "$out" --debug-flags=LVP,EgDiff \
        --debug-file="$out/simout" \
        configs/example/egdiff.py --cpu-type=O3CPU --caches \
        --cmd="$D/$name" > "$out/stdout" 2>&1
    local rc=$?
    [ $rc -eq 0 ] || { echo "  gem5 $name failed rc=$rc"; tail -20 "$out/stdout"; fail=$((fail+1)); }
}
OUT1="$WORK/test_layer2_spec_and_squash"
OUT2="$WORK/test_layer2_branch_squash"

run test_layer2_spec_and_squash test_layer2_spec_and_squash.c
run test_layer2_branch_squash  test_layer2_branch_squash.c

T1="$OUT1/simout"; S1="$OUT1/stats.txt"
T2="$OUT2/simout"; S2="$OUT2/stats.txt"

echo "== part 1: dispatch prediction, verification, blackout =="

RUNOUT=$(grep -a "layer2 sink=" "$OUT1/stdout" | tail -1)
case "$RUNOUT" in
  *" OK") ok "architectural checksum exact after squashes" ;;
  *)      bad "architectural checksum wrong: '$RUNOUT'" ;;
esac

cs "predictions were forwarded (/ deferred)" \
   "loadValuePredictor\.(egdiffPredictions|egdiffDeferredPredictions) +[1-9]" "$S1"
cs "at least one prediction verified correct" \
   "loadValuePredictor\.numPredictionCorrect +[1-9]" "$S1"

inc=$(awk '/^system\.cpu\.loadValuePredictor\.numPredictionIncorrect /{print $2}' "$S1")
if [ "${inc:-0}" = "1" ]; then ok "exactly one value misprediction"; else
    bad "expected numPredictionIncorrect=1, got '${inc:-missing}'"; fi
ct "misprediction triggered a squash" "EgDiff MISMATCH.*SQUASH" "$T1"

msn=$(grep -a "EgDiff MISMATCH" "$T1" | head -1 | sed -E 's/.*\[sn:([0-9]+)\].*/\1/')
if [ -n "${msn:-}" ]; then
    # Collect the sequence number at which every value is forwarded, from both
    # the normal dispatch path and the deferred wakeup path.
    # New predictions issued at dispatch.  A deferred publish for a load that
    # was already dispatched before the mismatch is allowed: the window
    # suppresses future predictions, not already-bound ones.
    issued_dispatch() {
        grep -aE "\[sn:[0-9]+\] dispatch PC .* predicts" "$T1" \
            | sed -E 's/^[0-9]+: .*\[sn:([0-9]+)\].*/\1/' | sort -n
    }
    issued_all() {
        { issued_dispatch
          grep -aE "deferred wakeup -> publish" "$T1" \
            | sed -E 's/.*\[dep:([0-9]+)\].*/\1/'; } | sort -n
    }
    inside=$(issued_dispatch | awk -v t="$msn" '$1 > t && $1 <= t+1024' | head -1)
    after=$(issued_all     | awk -v t="$msn" '$1 > t+1024' | head -1)
    [ -z "$inside" ] && ok "no value forwarded during the 1024-inst blackout" \
                     || bad "value forwarded at sn $inside inside the blackout"
    [ -n "$after" ]  && ok "predictions resumed after the blackout" \
                     || bad "predictions never resumed after the blackout"
else
    bad "could not find the mismatch sequence number"
fi
cs "suppression counter advanced" \
   "loadValuePredictor\.egdiffLastMispSuppressed +[1-9]" "$S1"

echo "== part 2: ROB-squash rollback =="

RUNOUT2=$(grep -a "layer2branch sink=" "$OUT2/stdout" | tail -1)
case "$RUNOUT2" in
  *" OK") ok "architectural checksum exact with wrong-path loads" ;;
  *)      bad "architectural checksum wrong: '$RUNOUT2'" ;;
esac
cs "ROB squashes discarded speculative entries" \
   "loadValuePredictor\.egdiffSquashed +[1-9]" "$S2"

echo
echo "== result: $pass passed, $fail failed =="
[ $fail -eq 0 ] || { echo "LAYER 2 FAILED"; exit 1; }
echo "LAYER 2 PASSED"
