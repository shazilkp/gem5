#!/bin/bash
#
# Layer 5 verification: real-library SipHash-2-4 key extraction.
#
# Builds tests/test-progs/egdiff_attack/siphash_attack.c, checks the compiled
# load topology with objdump, then runs three cases:
#
#   egdiff/on   -> must recover both 64-bit key words (16/16 bytes), MATCH
#   egdiff/off  -> must fail, and report exactly what it produced
#   flop/on     -> must fail, and report exactly what it produced
#
# The two controls must also leave every EgDiff statistic at zero and must
# produce the same architectural checksum (`sink`) as the EgDiff run.
#
# Usage: tests/test-progs/egdiff_attack/run_layer5.sh
#
# Environment:
#   GEM5    gem5 binary        (default build/RISCV/gem5.opt)
#   WORK    output directory   (default /tmp/opencode/egdiff_layer5)
#   OBJDUMP cross objdump      (default riscv64-linux-gnu-objdump)

set -u
cd "$(dirname "$0")/../../.." || exit 1

GEM5="${GEM5:-build/RISCV/gem5.opt}"
WORK="${WORK:-/tmp/opencode/egdiff_layer5}"
CC="${CC:-riscv64-linux-gnu-gcc}"
OBJDUMP="${OBJDUMP:-riscv64-linux-gnu-objdump}"
NM="${NM:-riscv64-linux-gnu-nm}"
D=tests/test-progs/egdiff_attack
BIN="$D/siphash_attack"

[ -x "$GEM5" ] || { echo "missing gem5: $GEM5"; exit 1; }

# The attacker-object ownership check below writes into $WORK before the
# per-case output directories are created.
mkdir -p "$WORK"

pass=0; fail=0
ok()  { echo "  PASS  $1"; pass=$((pass+1)); }
bad() { echo "  FAIL  $1"; fail=$((fail+1)); }

echo "== build =="
"$CC" -static -O2 -fno-pic -no-pie -o "$BIN" \
    "$D/siphash_attack.c" "$D/egdiff_secret.c" \
    || { echo "build failed"; exit 1; }
ok "compiled $BIN"

# The attacker translation unit must have no architectural reference to the
# SipHash key; only the helper (and the post-hoc oracle report) may read it.
if "$CC" -c -O2 -fno-pic -no-pie -o "$WORK/attacker_l5.o" "$D/siphash_attack.c" \
        2>/dev/null && ! "$NM" -u "$WORK/attacker_l5.o" | grep -q "secret_"; then
    ok "attacker TU has no reference to the SipHash key"
else
    bad "attacker TU references the SipHash key"
fi
if "$NM" "$BIN" | awk '$3 ~ /^secret_/ && $2 ~ /^[A-Z]$/ {found=1} END{exit !found}'; then
    bad "SipHash key is a global symbol (not static)"
else
    ok "SipHash key is static (unnameable from the attacker TU)"
fi

echo "== objdump load-topology check =="
SIP=$("$OBJDUMP" -d "$BIN" | sed -n '/<siphash24>:/,/^$/p')
LOADS=$(printf '%s\n' "$SIP" | grep -E '\b(ld|lw|lh|lhu|lb|lbu|lwu)\b')
L2=$(printf '%s\n' "$LOADS" | sed -n '2p')
L3=$(printf '%s\n' "$LOADS" | sed -n '3p')

if printf '%s' "$L2" | grep -qE 'ld[[:space:]]+[a-z0-9]+,0\(a2\)'; then
    ok "siphash key word k0 is load #2: $(printf '%s' "$L2" | sed 's/^ *//')"
else
    bad "siphash key word k0 is not the 2nd load"
    printf '    got: %s\n' "$L2"
fi
if printf '%s' "$L3" | grep -qE 'ld[[:space:]]+[a-z0-9]+,8\(a2\)'; then
    ok "siphash key word k1 is load #3: $(printf '%s' "$L3" | sed 's/^ *//')"
else
    bad "siphash key word k1 is not the 3rd load"
    printf '    got: %s\n' "$L3"
fi

# On the src_sz == 0 path the loop body (and its message loads) is skipped and
# the switch executes exactly one jump-table load before returning.  Check that
# the switch block contains exactly one load and that it is the jump-table lw.
SW=$(printf '%s\n' "$SIP" | sed -n '/and[[:space:]]*a1,a1,7/,/jr/p')
SWLOADS=$(printf '%s\n' "$SW" | grep -cE '\b(ld|lw|lh|lhu|lb|lbu|lwu)\b')
if [ "$SWLOADS" -eq 1 ] && printf '%s\n' "$SW" | grep -qE 'lw[[:space:]]+[a-z0-9]+,0\('; then
    ok "switch path executes exactly one load (jump-table lw)"
else
    bad "switch path has an unexpected number of loads ($SWLOADS)"
fi

# delayed_atk_load must contain no loads at all, so L_atk is the next dynamic
# load after the jump-table load.  Its 12 real divisions delay the address.
DELAY=$("$OBJDUMP" -d "$BIN" | sed -n '/<delayed_atk_load>:/,/<[a-z0-9_]*>:/p')
DELAY_LOADS=$(printf '%s\n' "$DELAY" | grep -cE '\b(ld|lw|lh|lhu|lb|lbu|lwu)\b')
DIVS=$(printf '%s\n' "$DELAY" | grep -cE '\bdivu\b')
if [ "$DELAY_LOADS" -eq 0 ] && [ "$DIVS" -ge 12 ]; then
    ok "delayed_atk_load has 0 loads and $DIVS divisions"
else
    bad "delayed_atk_load has $DELAY_LOADS loads / $DIVS divisions"
fi

ATK=$("$OBJDUMP" -d "$BIN" | sed -n '/<atk_load>:/,/^$/p')
ATK_LOADS=$(printf '%s\n' "$ATK" | grep -cE '\b(ld|lw|lh|lhu|lb|lbu|lwu)\b')
if [ "$ATK_LOADS" -eq 1 ]; then
    ok "atk_load is one static load (same PC for training and measurement)"
else
    bad "atk_load has $ATK_LOADS loads"
fi

echo "  distance: k0 = 6, k1 = 5 dynamic loads before L_atk (ORDER_N = 32)"

run_case() { # run_case <tag> <type> <enabled>
    local tag="$1" type="$2" on="$3"
    local out="$WORK/$tag"
    rm -rf "$out"; mkdir -p "$out"
    EGDIFF_TYPE="$type" EGDIFF_ON="$on" \
    timeout 3600 "$GEM5" -d "$out" \
        configs/example/egdiff.py --cpu-type=O3CPU --caches \
        --cmd="$PWD/$BIN" > "$out/stdout" 2>&1
    echo "  [$tag] $(grep -a 'rk0=' "$out/stdout" | tail -1)"
}

parse() { # parse <tag> <field>
    local out="$WORK/$1/stdout" f="$2"
    sed -n "s/.* $f=\\([0-9a-f]*\\).*/\\1/p" "$out" | tail -1
}

echo "== case 1: EgDiff on =="
run_case on egdiff 1
ON_LINE=$(grep -a 'rk0=' "$WORK/on/stdout" | tail -1)
K0=$(parse on k0); K1=$(parse on k1)
RK0=$(parse on rk0); RK1=$(parse on rk1)
BYTES=$(sed -n 's/.* bytes=\([0-9]*\)\/16.*/\1/p' <<<"$ON_LINE")
ARCH=$(sed -n 's/.* arch=\([A-Z]*\).*/\1/p' <<<"$ON_LINE")
VERD=$(awk '{print $NF}' <<<"$ON_LINE")
SINK_ON=$(sed -n 's/.* sink=\([0-9]*\).*/\1/p' <<<"$ON_LINE")

[ "$VERD" = "MATCH" ] && ok "egdiff/on verdict MATCH" || bad "egdiff/on verdict '$VERD'"
[ "$ARCH" = "OK" ] && ok "egdiff/on architectural precondition holds" || bad "egdiff/on arch=$ARCH"
[ "$BYTES" = "16" ] && ok "egdiff/on recovered 16/16 bytes" || bad "egdiff/on recovered $BYTES/16 bytes"
[ -n "$K0" ] && [ "$RK0" = "$K0" ] && ok "k0 $K0 recovered exactly" || bad "k0 $K0 -> '$RK0'"
[ -n "$K1" ] && [ "$RK1" = "$K1" ] && ok "k1 $K1 recovered exactly" || bad "k1 $K1 -> '$RK1'"
grep -aq "loadValuePredictor.egdiffPredictions \+[1-9]" "$WORK/on/stats.txt" \
    && ok "egdiff/on: EgDiff forwarded values (egdiffPredictions > 0)" \
    || bad "egdiff/on: no EgDiff predictions"

echo "== case 2: EgDiff off (negative control) =="
run_case off egdiff 0
OFF_LINE=$(grep -a 'rk0=' "$WORK/off/stdout" | tail -1)
OFF_VERD=$(awk '{print $NF}' <<<"$OFF_LINE")
OFF_ARCH=$(sed -n 's/.* arch=\([A-Z]*\).*/\1/p' <<<"$OFF_LINE")
OFF_RK0=$(parse off rk0); OFF_RK1=$(parse off rk1)
SINK_OFF=$(sed -n 's/.* sink=\([0-9]*\).*/\1/p' <<<"$OFF_LINE")
echo "  [off] produced rk0=$OFF_RK0 rk1=$OFF_RK1"
[ "$OFF_VERD" = "NO-MATCH" ] && ok "egdiff/off verdict NO-MATCH" || bad "egdiff/off verdict '$OFF_VERD'"
[ "$OFF_ARCH" = "OK" ] && ok "egdiff/off architectural precondition holds" || bad "egdiff/off arch=$OFF_ARCH"
if [ "$OFF_RK0" != "$K0" ] || [ "$OFF_RK1" != "$K1" ]; then
    ok "egdiff/off did not reproduce the key"
else
    bad "egdiff/off reproduced the key"
fi
OFF_STATS=$(grep -E "loadValuePredictor\.egdiff" "$WORK/off/stats.txt" \
            | awk '{s += $2} END {print s+0}')
[ "$OFF_STATS" = "0" ] && ok "egdiff/off: every EgDiff statistic is zero" \
                       || bad "egdiff/off: EgDiff statistics non-zero ($OFF_STATS)"

echo "== case 3: FLOP local LVP (negative control) =="
run_case flop flop 1
FLOP_LINE=$(grep -a 'rk0=' "$WORK/flop/stdout" | tail -1)
FLOP_VERD=$(awk '{print $NF}' <<<"$FLOP_LINE")
FLOP_ARCH=$(sed -n 's/.* arch=\([A-Z]*\).*/\1/p' <<<"$FLOP_LINE")
FLOP_RK0=$(parse flop rk0); FLOP_RK1=$(parse flop rk1)
SINK_FLOP=$(sed -n 's/.* sink=\([0-9]*\).*/\1/p' <<<"$FLOP_LINE")
echo "  [flop] produced rk0=$FLOP_RK0 rk1=$FLOP_RK1"
[ "$FLOP_VERD" = "NO-MATCH" ] && ok "flop verdict NO-MATCH" || bad "flop verdict '$FLOP_VERD'"
[ "$FLOP_ARCH" = "OK" ] && ok "flop architectural precondition holds" || bad "flop arch=$FLOP_ARCH"
if [ "$FLOP_RK0" != "$K0" ] || [ "$FLOP_RK1" != "$K1" ]; then
    ok "flop did not reproduce the key"
else
    bad "flop reproduced the key"
fi
FLOP_STATS=$(grep -E "loadValuePredictor\.egdiff" "$WORK/flop/stats.txt" \
             | awk '{s += $2} END {print s+0}')
[ "$FLOP_STATS" = "0" ] && ok "flop: every EgDiff statistic is zero" \
                        || bad "flop: EgDiff statistics non-zero ($FLOP_STATS)"
FLOP_PRED=$(grep -E "loadValuePredictor\.numPredictions " "$WORK/flop/stats.txt" \
            | awk '{print $2}')
[ "${FLOP_PRED:-0}" -gt 0 ] && ok "flop: the FLOP predictor itself is active" \
                            || bad "flop: FLOP predictor inactive"

echo "== architectural equivalence =="
if [ "$SINK_ON" = "$SINK_OFF" ] && [ "$SINK_ON" = "$SINK_FLOP" ]; then
    ok "identical architectural checksum sink=$SINK_ON in all three runs"
else
    bad "sink differs: on=$SINK_ON off=$SINK_OFF flop=$SINK_FLOP"
fi

echo
echo "== result: $pass passed, $fail failed =="
[ $fail -eq 0 ] || { echo "LAYER 5 FAILED"; exit 1; }
echo "LAYER 5 PASSED"
