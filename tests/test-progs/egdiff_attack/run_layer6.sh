#!/bin/bash
#
# Layer 6 / experiment A4 verification: EgDiff forwards a byte of Monocypher's
# crypto_verify32 key across the call boundary.
#
# Pipeline:
#   1. build poc_verify32.c + target_verify32.c + the real Monocypher source
#   2. ownership checks (the attacker TU cannot name the key)
#   3. static load-topology checks (objdump)
#   4. measure d in *this* binary with qemu -singlestep -d exec + objdump
#      (measure_d.py), and take the measured (key offset, d) map
#   5. run three cases: egdiff/on, egdiff/off, flop/on
#   6. assert recovery, controls, statistics, and architectural equivalence
#
# Usage: tests/test-progs/egdiff_attack/run_layer6.sh
#
# Environment:
#   GEM5     gem5 binary       (default build/RISCV/gem5.opt)
#   WORK     output directory  (default /tmp/opencode/egdiff_layer6)
#   CC/OBJDUMP/NM/QEMU         cross toolchain (default riscv64-linux-gnu-*)
#   MONO     Monocypher src dir (default prevalence vendored copy)
#   PYTHON   python interpreter (default python3)

set -u
cd "$(dirname "$0")/../../.." || exit 1

GEM5="${GEM5:-build/RISCV/gem5.opt}"
WORK="${WORK:-/tmp/opencode/egdiff_layer6}"
CC="${CC:-riscv64-linux-gnu-gcc}"
OBJDUMP="${OBJDUMP:-riscv64-linux-gnu-objdump}"
NM="${NM:-riscv64-linux-gnu-nm}"
QEMU="${QEMU:-qemu-riscv64}"
PYTHON="${PYTHON:-python3}"
MONO="${MONO:-experiment/egdiff_eval/prevalence/third_party/Monocypher/src}"
D=tests/test-progs/egdiff_attack
BIN="$D/poc_verify32"
# The paper's prediction text uses the speculative queue.  The A4 gate is
# strict by default (no NVQ base); set EGDIFF_NVQ_BASE=1 for the extension.
NVQ_BASE="${EGDIFF_NVQ_BASE:-0}"

[ -x "$GEM5" ] || { echo "missing gem5: $GEM5"; exit 1; }
[ -f "$MONO/monocypher.c" ] || { echo "missing Monocypher: $MONO"; exit 1; }
mkdir -p "$WORK"

pass=0; fail=0
ok()  { echo "  PASS  $1"; pass=$((pass+1)); }
bad() { echo "  FAIL  $1"; fail=$((fail+1)); }

echo "== build =="
"$CC" -static -O2 -fno-pic -no-pie -fno-ipa-icf -I"$MONO" -o "$BIN" \
    "$D/poc_verify32.c" "$D/target_verify32.c" "$MONO/monocypher.c" \
    || { echo "build failed"; exit 1; }
ok "compiled $BIN (attacker + target + real monocypher.c)"

echo "== ownership split =="
if "$CC" -c -O2 -fno-pic -no-pie -I"$MONO" -o "$WORK/attacker_a4.o" \
       "$D/poc_verify32.c" 2>/dev/null \
   && ! "$NM" -u "$WORK/attacker_a4.o" | grep -Eq 'secret_|v32_key'; then
    ok "attacker TU has no undefined reference to the key symbol"
else
    bad "attacker TU references the key symbol"
fi
if "$NM" "$BIN" | awk '$3 ~ /^(secret_|v32_key)/ && $2 ~ /^[A-Z]$/ {found=1} END{exit !found}'; then
    bad "key is a global symbol (not static)"
else
    ok "key is not a global symbol (static, unnameable from the attacker TU)"
fi
if "$NM" "$BIN" | grep -qE '^[0-9a-f]+ r v32_key_storage$'; then
    ok "v32_key_storage is a local read-only symbol"
else
    bad "v32_key_storage not found as a local symbol"
fi

echo "== static load topology =="
DIS=$("$OBJDUMP" -d --no-show-raw-insn "$BIN")
fn() { printf '%s\n' "$DIS" | sed -n "/<$1>:/,/^\$/p"; }
LOADRE='\b(ld|lw|lh|lhu|lb|lbu|lwu)\b'

DELAY=$(fn delayed_atk_load)
DELAY_LOADS=$(printf '%s\n' "$DELAY" | grep -cE "$LOADRE")
DELAY_DIVS=$(printf '%s\n' "$DELAY" | grep -cE '\bdivu\b')
if [ "$DELAY_LOADS" -eq 0 ] && [ "$DELAY_DIVS" -ge 12 ]; then
    ok "delayed_atk_load has 0 loads and $DELAY_DIVS register-only divisions"
else
    bad "delayed_atk_load has $DELAY_LOADS loads / $DELAY_DIVS divisions"
fi

ATK_LOADS=$(fn atk_load | grep -cE "$LOADRE")
if [ "$ATK_LOADS" -eq 1 ]; then
    ok "atk_load is one static load (same PC for training and measurement)"
else
    bad "atk_load has $ATK_LOADS loads"
fi

VTM=$(fn victim_then_marker)
# The load window is the instruction range between the call to the target and
# the call that performs the marker load.  (victim_then_marker's own `ld ra`
# frame restore is emitted *after* the marker call, so it is outside the window.)
VTM_WINDOW=$(printf '%s\n' "$VTM" \
    | sed -n '/<target_verify32>/,/<delayed_atk_load>/p' | sed '1d;$d')
VTM_LOADS=$(printf '%s\n' "$VTM_WINDOW" | grep -cE "$LOADRE")
if [ -n "$VTM_WINDOW" ] && [ "$VTM_LOADS" -eq 0 ]; then
    ok "0 loads between the target call and the marker call (objdump)"
else
    bad "$VTM_LOADS load(s) between the target call and the marker call"
    printf '%s\n' "$VTM_WINDOW"
fi

TV=$(fn target_verify32)
if printf '%s\n' "$TV" | grep -q 'v32_key_storage' \
   && printf '%s\n' "$TV" | grep -q 'crypto_verify32'; then
    ok "target_verify32 passes v32_key_storage to crypto_verify32"
else
    bad "target_verify32 does not pass the key to crypto_verify32"
fi

echo "== measured distances (qemu -singlestep -d exec + objdump) =="
MEASURE=$("$PYTHON" "$D/measure_d.py" "$BIN" --qemu "$QEMU" \
          --objdump "$OBJDUMP" 2>&1)
MRC=$?
printf '%s\n' "$MEASURE" | tee "$WORK/measure.txt"
if [ $MRC -ne 0 ]; then
    bad "measure_d.py failed"
    echo
    echo "== result: $pass passed, $fail failed =="
    echo "LAYER 6 FAILED"; exit 1
fi
ok "measure_d.py measured the last-key-word distances"

LANE_BASE=$(sed -n 's/^LANE_BASE //p' <<<"$MEASURE")
ND=$(grep -c '^D ' <<<"$MEASURE")
BRET=$(sed -n 's/.*loads_between_ret_and_marker=\([0-9]*\).*/\1/p' <<<"$MEASURE")
[ "$BRET" = "0" ] && ok "0 loads between the callee ret and the marker" \
                   || bad "loads between callee ret and marker: '$BRET'"
[ "$ND" = "8" ] && ok "8 last-key-word byte loads traced" \
                 || bad "traced $ND/8 key byte loads"
[ "$LANE_BASE" = "24" ] && ok "last key word is at key offset 24..31" \
                        || bad "last key word base is offset '$LANE_BASE' (want 24)"

OFFSETS=$(grep '^D ' <<<"$MEASURE" | awk '{print $2}' | sort -n | tr '\n' ' ')
[ "$OFFSETS" = "24 25 26 27 28 29 30 31 " ] \
    && ok "measured offsets cover the whole last word (24..31)" \
    || bad "measured offsets: '$OFFSETS'"
if grep '^D ' <<<"$MEASURE" | awk '$3 > 32 || $3 < 1 {bad=1} END{exit !bad}'; then
    bad "a measured d is outside EgDiff's 1..32 window"
else
    ok "every measured d is inside EgDiff's 1..32 window"
fi

SPEC=$(grep '^D ' <<<"$MEASURE" | awk 'BEGIN{n=0}{printf "%s%s:%s", (n++?",":""), $2, $3}')
echo "  measured map: $SPEC"

echo "== case 1: EgDiff on =="
run_case() { # run_case <tag> <type> <enabled>
    local tag="$1" type="$2" on="$3"
    local out="$WORK/$tag"
    rm -rf "$out"; mkdir -p "$out"
    EGDIFF_TYPE="$type" EGDIFF_ON="$on" EGDIFF_NVQ_BASE="$NVQ_BASE" \
    timeout 1800 "$GEM5" -d "$out" \
        configs/example/egdiff.py --cpu-type=O3CPU --caches \
        --cmd="$PWD/$BIN" --options="attack $SPEC" > "$out/stdout" 2>&1 || true
    echo "  [$tag] $(grep -a 'verify32_attack key_word=' "$out/stdout" | tail -1)"
}
field() { sed -n "s/.* $2=\([0-9a-f]*\).*/\1/p" <<<"$1" | tail -1; }
run_case on egdiff 1
ON=$(grep -a 'verify32_attack key_word=' "$WORK/on/stdout" | tail -1)
ON_KW=$(field "$ON" key_word); ON_REC=$(field "$ON" recovered)
ON_KNOWN=$(field "$ON" known); ON_ARCH=$(sed -n 's/.* arch=\([A-Z]*\).*/\1/p' <<<"$ON")
ON_BYTES=$(sed -n 's/.* bytes=\([0-9]*\)\/8.*/\1/p' <<<"$ON")
ON_VERD=$(awk '{print $NF}' <<<"$ON")
ON_SINK=$(field "$ON" sink)
ON_LANES=$(grep -ac 'lane key_off=.* got=' "$WORK/on/stdout")
ON_VOTES=$(grep -a 'lane key_off=' "$WORK/on/stdout" | sed -n 's/.* votes=\([0-9]*\).*/\1/p' | sort -n | head -1)
ONPRE=$(grep -a 'poc_verify32 archpre' "$WORK/on/stdout" | tail -1)

[ "$ON_VERD" = "MATCH" ] && ok "egdiff/on verdict MATCH" || bad "egdiff/on verdict '$ON_VERD'"
[ "$ON_KNOWN" = "ff" ] && ok "egdiff/on known=ff" || bad "egdiff/on known=$ON_KNOWN"
[ "$ON_BYTES" = "8" ] && ok "egdiff/on recovered 8/8 bytes" || bad "egdiff/on recovered $ON_BYTES/8"
[ "$ON_LANES" = "8" ] && ok "egdiff/on all 8 lanes produced a byte" \
                       || bad "egdiff/on only $ON_LANES/8 lanes produced a byte"
[ "${ON_VOTES:-0}" -ge 2 ] && ok "egdiff/on minimum lane votes ${ON_VOTES} >= 2" \
                           || bad "egdiff/on minimum lane votes ${ON_VOTES:-0}"
[ -n "$ON_KW" ] && [ "$ON_REC" = "$ON_KW" ] \
    && ok "egdiff/on recovered word $ON_REC exactly" \
    || bad "egdiff/on key word $ON_KW -> '$ON_REC'"
[ "$ON_ARCH" = "OK" ] && ok "egdiff/on arch=OK" || bad "egdiff/on arch=$ON_ARCH"
case "$ONPRE" in
    *self=OK*repro=OK*marker=OK*) ok "egdiff/on arch precondition: self/repro/marker all OK" ;;
    *) bad "egdiff/on arch precondition line: '$ONPRE'" ;;
esac
grep -aq "loadValuePredictor\.egdiffPredictions \+[1-9]" "$WORK/on/stats.txt" \
    && ok "egdiff/on: EgDiff forwarded values (egdiffPredictions > 0)" \
    || bad "egdiff/on: no EgDiff predictions"
grep -aq "loadValuePredictor\.numPredictionIncorrect \+[1-9]" "$WORK/on/stats.txt" \
    && ok "egdiff/on: the measured loads mispredicted (squash path exercised)" \
    || bad "egdiff/on: no incorrect predictions"

echo "== case 2: EgDiff off (negative control) =="
run_case off egdiff 0
OFF=$(grep -a 'verify32_attack key_word=' "$WORK/off/stdout" | tail -1)
OFF_KNOWN=$(field "$OFF" known); OFF_ARCH=$(sed -n 's/.* arch=\([A-Z]*\).*/\1/p' <<<"$OFF")
OFF_BYTES=$(sed -n 's/.* bytes=\([0-9]*\)\/8.*/\1/p' <<<"$OFF")
OFF_VERD=$(awk '{print $NF}' <<<"$OFF")
OFF_SINK=$(field "$OFF" sink)
[ "$OFF_VERD" = "NO-MATCH" ] && ok "egdiff/off verdict NO-MATCH" || bad "egdiff/off verdict '$OFF_VERD'"
[ "$OFF_KNOWN" = "00" ] && ok "egdiff/off known=00" || bad "egdiff/off known=$OFF_KNOWN"
[ "$OFF_BYTES" = "0" ] && ok "egdiff/off recovered 0/8 bytes" || bad "egdiff/off recovered $OFF_BYTES/8"
[ "$OFF_ARCH" = "OK" ] && ok "egdiff/off arch=OK" || bad "egdiff/off arch=$OFF_ARCH"
OFF_STATS=$(grep -E "loadValuePredictor\.egdiff" "$WORK/off/stats.txt" \
            | awk '{s += $2} END {print s+0}')
[ "$OFF_STATS" = "0" ] && ok "egdiff/off: every EgDiff statistic is zero" \
                       || bad "egdiff/off: EgDiff statistics non-zero ($OFF_STATS)"

echo "== case 3: FLOP local LVP (negative control) =="
run_case flop flop 1
FLOP=$(grep -a 'verify32_attack key_word=' "$WORK/flop/stdout" | tail -1)
FLOP_KNOWN=$(field "$FLOP" known); FLOP_ARCH=$(sed -n 's/.* arch=\([A-Z]*\).*/\1/p' <<<"$FLOP")
FLOP_BYTES=$(sed -n 's/.* bytes=\([0-9]*\)\/8.*/\1/p' <<<"$FLOP")
FLOP_VERD=$(awk '{print $NF}' <<<"$FLOP")
FLOP_SINK=$(field "$FLOP" sink)
[ "$FLOP_VERD" = "NO-MATCH" ] && ok "flop verdict NO-MATCH" || bad "flop verdict '$FLOP_VERD'"
[ "$FLOP_KNOWN" = "00" ] && ok "flop known=00" || bad "flop known=$FLOP_KNOWN"
[ "$FLOP_BYTES" = "0" ] && ok "flop recovered 0/8 bytes" || bad "flop recovered $FLOP_BYTES/8"
[ "$FLOP_ARCH" = "OK" ] && ok "flop arch=OK" || bad "flop arch=$FLOP_ARCH"
FLOP_STATS=$(grep -E "loadValuePredictor\.egdiff" "$WORK/flop/stats.txt" \
             | awk '{s += $2} END {print s+0}')
[ "$FLOP_STATS" = "0" ] && ok "flop: every EgDiff statistic is zero" \
                        || bad "flop: EgDiff statistics non-zero ($FLOP_STATS)"
FLOP_PRED=$(grep -E "loadValuePredictor\.numPredictions " "$WORK/flop/stats.txt" \
            | awk '{print $2}')
[ "${FLOP_PRED:-0}" -gt 0 ] && ok "flop: the FLOP predictor itself is active" \
                            || bad "flop: FLOP predictor inactive"

echo "== architectural equivalence =="
if [ "$ON_SINK" = "$OFF_SINK" ] && [ "$ON_SINK" = "$FLOP_SINK" ]; then
    ok "identical architectural checksum sink=$ON_SINK in all three runs"
else
    bad "sink differs: on=$ON_SINK off=$OFF_SINK flop=$FLOP_SINK"
fi

echo
echo "== result: $pass passed, $fail failed =="
[ $fail -eq 0 ] || { echo "LAYER 6 FAILED"; exit 1; }
echo "LAYER 6 PASSED"
