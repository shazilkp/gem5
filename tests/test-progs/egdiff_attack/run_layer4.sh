#!/bin/bash
#
# Layer 4 verification: two conformance experiments for the EgDiff model.
#
#   Mode 1 (poc_v1_specgvq_1shot)  in-flight speculative-queue prediction
#   Mode 2 (poc_v2_nvq_fence_bypass) commit-time NVQ capture + replay
#
# For each mode: EgDiff on must recover the full 64-bit value; the predictor
# disabled and the FLOP local predictor must both fail.
#
# Usage: tests/test-progs/egdiff_attack/run_layer4.sh
#
# Environment:
#   GEM5    gem5 binary        (default build/RISCV/gem5.opt)
#   WORK    output directory   (default /tmp/opencode/egdiff_layer4)
#   ONLY    "1" or "2" to run a single mode
#   SECRET  Mode 1 only: 64-bit value to extract (default: the PoC's own)

set -u
cd "$(dirname "$0")/../../.." || exit 1

GEM5="${GEM5:-build/RISCV/gem5.opt}"
WORK="${WORK:-/tmp/opencode/egdiff_layer4}"
CC="${CC:-riscv64-linux-gnu-gcc}"
D=tests/test-progs/egdiff_attack
ONLY="${ONLY:-}"
SECRET="${SECRET:-}"
DEFS=""
[ -n "$SECRET" ] && DEFS="-DSECRET_VAL=${SECRET}ULL"

[ -x "$GEM5" ] || { echo "missing gem5: $GEM5"; exit 1; }

pass=0; fail=0
ok()  { echo "  PASS  $1"; pass=$((pass+1)); }
bad() { echo "  FAIL  $1"; fail=$((fail+1)); }

run_case() { # run_case <name> <src> <type> <enabled> <tag> <verdict-token> <dst-gate>
    local name="$1" src="$2" type="$3" on="$4" tag="$5" want="$6" dst="${7:-}"
    local bin="$D/$name${SECRET:+.${SECRET}}"
    "$CC" -static -O2 -fno-pic -no-pie $DEFS -o "$bin" "$D/$src" || return 1
    local out="$WORK/$name.$tag"
    rm -rf "$out"; mkdir -p "$out"
    EGDIFF_TYPE="$type" EGDIFF_ON="$on" \
    timeout 1800 "$GEM5" -d "$out" \
        configs/example/egdiff.py --cpu-type=O3CPU --caches \
        --cmd="$bin" > "$out/stdout" 2>&1
    local line verdict
    line=$(grep -a "secret=" "$out/stdout" | tail -1)
    echo "  [$tag] $line"
    verdict=$(printf '%s\n' "$line" | awk '{print $NF}')
    if [ "$verdict" = "$want" ]; then
        ok "$name/$tag: $want"
    else
        bad "$name/$tag: expected $want, got '${verdict:-none}'"
    fi
    # Mode 1 self-description gate: how many bytes the harness claims to have
    # measured.  EgDiff must claim all 8 (known=ff); a control must claim none
    # (known=00), which is what makes its NO-MATCH meaningful rather than a
    # fabricated default.
    if [ -n "$dst" ]; then
        if printf '%s\n' "$line" | grep -q "$dst"; then
            ok "$name/$tag: harness reports $dst"
        else
            bad "$name/$tag: harness did not report $dst"
        fi
    fi
}

echo "== Mode 1: in-flight speculative-queue prediction =="
if [ -z "$ONLY" ] || [ "$ONLY" = "1" ]; then
    run_case poc_v1_specgvq_1shot poc_v1_specgvq_1shot.c egdiff 1 on  MATCH    known=ff
    run_case poc_v1_specgvq_1shot poc_v1_specgvq_1shot.c egdiff 0 off NO-MATCH known=00
    run_case poc_v1_specgvq_1shot poc_v1_specgvq_1shot.c flop   1 flop NO-MATCH known=00
    grep -aq "loadValuePredictor\.egdiffDeferredPredictions \+[1-9]" \
        "$WORK/poc_v1_specgvq_1shot.on/stats.txt" \
        && ok "mode 1: deferred (Wake) predictions fired" \
        || bad "mode 1: no deferred predictions"
fi

echo "== Mode 2: commit-time NVQ capture + replay =="
if [ -z "$ONLY" ] || [ "$ONLY" = "2" ]; then
    run_case poc_v2_nvq_fence_bypass poc_v2_nvq_fence_bypass.c egdiff 1 on  MATCH
    run_case poc_v2_nvq_fence_bypass poc_v2_nvq_fence_bypass.c egdiff 0 off NO-MATCH
    run_case poc_v2_nvq_fence_bypass poc_v2_nvq_fence_bypass.c flop   1 flop NO-MATCH
    # The harness asserts its own architectural precondition; require it.
    grep -aq "arch=OK" "$WORK/poc_v2_nvq_fence_bypass.on/stdout" \
        && ok "mode 2: architectural precondition holds" \
        || bad "mode 2: architectural precondition failed"
    grep -aq "loadValuePredictor\.egdiffNVQBases \+[1-9]" \
        "$WORK/poc_v2_nvq_fence_bypass.on/stats.txt" \
        && ok "mode 2: committed-NVQ base predictions occurred" \
        || bad "mode 2: no committed-NVQ base predictions"
fi

echo
echo "== result: $pass passed, $fail failed =="
[ $fail -eq 0 ] || { echo "LAYER 4 FAILED"; exit 1; }
echo "LAYER 4 PASSED"
