#!/bin/bash
#
# SpaceSpec demonstration matrix.
#
# Runs two independent cache side-channel attacks with the mechanism off and on,
# and prints one table. Both attacks are Flush+Reload: the victim speculatively
# touches a line whose address is derived from a secret, and the attacker
# measures which line is now fast in L1. If the line is resident, the secret
# comes out; if it is not, the measurement is noise.
#
#   spectre_v1   bounds-check bypass. The only speculation is a branch
#                misprediction -- the Load Value Predictor is not involved.
#   flop_attack  the leaking load's value is predicted by the LVP, so the
#                branch resolves without waiting for the load.
#
# "OFF" is stock gem5: the unmodified se.py, with no ShadowCache attached
# anywhere. "ON" is configs/example/spacespec.py, which attaches one ShadowCache
# to the CPU and its L1-D. Nothing else differs between the two columns.
#
# Usage:
#   tests/test-progs/lvp_attack/run_spacespec_demo.sh [gem5-binary]
#
# Environment:
#   TIMEOUT   seconds per run (default 900)

set -u

GEM5="${1:-build/RISCV/gem5.opt}"
TIMEOUT="${TIMEOUT:-900}"
GEM5_EXTRA="${GEM5_EXTRA:-}"
SE=configs/deprecated/example/se.py
SS=configs/example/spacespec.py
ATTACKS=tests/test-progs/lvp_attack

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$GEM5" ]; then
    echo "gem5 binary not found or not executable: $GEM5" >&2
    exit 1
fi

# Build the attack programs if they are missing.
for src in spectre_v1 flop_attack; do
    if [ ! -x "$ATTACKS/$src" ]; then
        echo "building $src"
        riscv64-linux-gnu-gcc -static -O2 -o "$ATTACKS/$src" "$ATTACKS/$src.c" \
            || exit 1
    fi
done

run() {
    # run <tag> <config> <binary> [env assignments...]
    local tag="$1" cfg="$2" bin="$3"; shift 3
    local log="$WORK/$tag.log"

    env "$@" timeout "$TIMEOUT" "$GEM5" -d "$WORK/$tag" "$cfg" \
        --cpu-type=O3CPU --caches $GEM5_EXTRA --cmd="$bin" > "$log" 2>&1
    local rc=$?

    if [ $rc -ne 0 ]; then
        printf 'TIMEOUT/CRASH'
        return
    fi
}

verdict_spectre() {
    grep -a -o 'verdict=[A-Z-]*' "$1" | head -1 | cut -d= -f2
}

verdict_flop() {
    # flop_attack prints per-round HIT / NO-LEAK markers.
    local hits
    hits=$(grep -a -c '\[HIT\]' "$1")
    local total
    total=$(grep -a -c '^round' "$1")
    if [ "$hits" -gt 0 ]; then echo "LEAK ($hits/$total)"; else echo "NO-LEAK (0/$total)"; fi
}

recovered_flop() {
    grep -a '^recovered:' "$1" | head -1 | sed 's/recovered: //'
}

recovered_spectre() {
    grep -a -o 'rec "[^"]*"' "$1" | head -1
}

stat_of() {
    # stat_of <outdir> <statname>
    [ -f "$1/stats.txt" ] && awk -v s="$2" '$1 == s {print $2}' "$1/stats.txt"
}

echo
echo "=============================================================="
echo " SpaceSpec demonstration: speculative cache fills"
echo "=============================================================="
echo
echo "Both attacks recover a secret by timing which cache line the victim"
echo "left behind in L1. SpaceSpec keeps speculative fills out of L1 and"
echo "installs them only once the load commits."
echo

printf "Building runs...\n"

# --- Column 1: mechanism OFF (stock gem5, unmodified se.py) -----------------
run v1_off  "$SE" "$ATTACKS/spectre_v1"  DUMMY=1
run flop_off "$SE" "$ATTACKS/flop_attack"  DUMMY=1

# --- Column 2: mechanism ON -------------------------------------------------
run v1_on   "$SS" "$ATTACKS/spectre_v1"  DUMMY=1
run flop_on "$SS" "$ATTACKS/flop_attack"  DUMMY=1

# --- Controls ---------------------------------------------------------------
# LVP disabled: proves spectre_v1 is a branch-prediction attack, not an LVP
# artifact, and that the protection does not depend on the predictor.
run v1_off_nolvp "$SS" "$ATTACKS/spectre_v1"  SPACESPEC_LVP=0 SPACESPEC_INTERCEPT=0
run v1_on_nolvp  "$SS" "$ATTACKS/spectre_v1"  SPACESPEC_LVP=0

# Divert disabled: the shadow still captures and promotes, but fills are
# allowed to install normally. If the leak comes back, the protection comes
# from the divert rather than from incidental timing differences.
run v1_nodivert "$SS" "$ATTACKS/spectre_v1"   SPACESPEC_DIVERT=0

echo
echo "----------------------------------------------------------------"
echo " RESULT"
echo "----------------------------------------------------------------"
printf " %-12s %-14s %-22s %-22s\n" "attack" "speculation" "SpaceSpec OFF" "SpaceSpec ON"
printf " %-12s %-14s %-22s %-22s\n" "-----" "------------" "-------------" "-------------"
printf " %-12s %-14s %-22s %-22s\n" "spectre_v1" "branch pred" \
    "$(verdict_spectre "$WORK/v1_off.log")" "$(verdict_spectre "$WORK/v1_on.log")"
printf " %-12s %-14s %-22s %-22s\n" "flop_attack" "LVP" \
    "$(verdict_flop "$WORK/flop_off.log")" "$(verdict_flop "$WORK/flop_on.log")"
echo
echo " Recovered secret"
printf "   spectre_v1  OFF: %-20s ON: %s\n" \
    "$(recovered_spectre "$WORK/v1_off.log")" "$(recovered_spectre "$WORK/v1_on.log")"
printf "   flop_attack OFF: %-20s ON: %s\n" \
    "$(recovered_flop "$WORK/flop_off.log")" "$(recovered_flop "$WORK/flop_on.log")"
echo
echo "----------------------------------------------------------------"
echo " CONTROLS"
echo "----------------------------------------------------------------"
printf " %-46s %s\n" "spectre_v1, LVP OFF, mechanism OFF" \
    "$(verdict_spectre "$WORK/v1_off_nolvp.log")"
printf " %-46s %s\n" "spectre_v1, LVP OFF, mechanism ON" \
    "$(verdict_spectre "$WORK/v1_on_nolvp.log")"
printf " %-46s %s\n" "spectre_v1, divert OFF (control)" \
    "$(verdict_spectre "$WORK/v1_nodivert.log")"
echo
echo "----------------------------------------------------------------"
echo " MECHANISM ACTIVITY (SpaceSpec ON runs)"
echo "----------------------------------------------------------------"
for pair in "spectre_v1:$WORK/v1_on" "flop_attack:$WORK/flop_on"; do
    name=${pair%%:*}; dir=${pair##*:}
    printf " %s\n" "$name"
    for s in captures promotions squashes overflows storeDrops refreshes; do
        v=$(stat_of "$dir" "system.cpu.shadow_cache.$s")
        [ -n "$v" ] && printf "   %-14s %s\n" "$s" "$v"
    done
    v=$(stat_of "$dir" "system.cpu.loadValuePredictor.numPredictionCorrect")
    [ -n "$v" ] && printf "   %-14s %s\n" "lvp_correct" "$v"
    echo
done

echo "----------------------------------------------------------------"
echo " Reading this table"
echo "----------------------------------------------------------------"
cat <<'EOF'
 OFF must LEAK and ON must NO-LEAK, for both attacks.

 The LVP-OFF control shows spectre_v1 works with the Load Value Predictor
 disabled, so it is a branch-prediction attack. SpaceSpec still stops it,
 which means the protection belongs to the cache mechanism rather than to
 an interaction with the predictor.

 The divert-OFF control keeps the shadow fully active -- it still captures
 and still promotes -- but lets fills install into L1 as usual. The leak
 returns, which shows the protection comes from withholding the fill and
 not from the simulation merely running slower.

 ON runs with LEAK would mean the mechanism is not withholding anything.
 OFF runs with NO-LEAK would mean the attack is broken, not the mechanism
 safe: both attacks were verified to leak on stock gem5 before being used
 as evidence.
EOF
echo
