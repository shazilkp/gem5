#!/bin/bash
#
# SpaceSpec correctness harness.
#
# Runs each test program twice -- once with the LVP only (baseline) and once
# with SpaceSpec added on top -- and requires byte-identical output.
#
# Why output equivalence rather than "did it crash": a fault deep in a
# program is often thousands of instructions from its cause, so the reported
# location misdirects. Output equality localises a regression to whichever
# layer broke it, because a layer that preserves output cannot have broken
# it. Crashing is just one way to fail, not the only signal.
#
# Two categories of test, and they have different pass conditions:
#
#   output-equivalence  prints only deterministic values. Must match
#                       exactly. This is the primary correctness signal.
#   security            measures a side channel (prints cycle counts).
#                       Its pass condition is that the LEAK DISAPPEARS,
#                       which cannot be expressed as an output diff.
#
# Usage:
#   tests/test-progs/spacespec_test/run_harness.sh <gem5-binary> [outdir]
#
# Environment:
#   SPACESPEC_CONFIG   config script for the SpaceSpec run
#                      (default configs/example/spacespec.py)
#   TIMEOUT            seconds per run (default 300)

set -u

GEM5="${1:-build/RISCV/gem5.opt}"
OUTDIR="${2:-/tmp/spacespec_harness}"
CONFIG="${SPACESPEC_CONFIG:-configs/example/spacespec.py}"
TIMEOUT="${TIMEOUT:-300}"

SE=configs/deprecated/example/se.py
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD=/tmp/spacespec_bin

if [ ! -x "$GEM5" ]; then
    echo "gem5 binary not found: $GEM5" >&2
    exit 2
fi

mkdir -p "$BUILD" "$OUTDIR"

# Build every test program.
TESTS="store pressure atomic chain deferred split shadowhit"
echo "== building test programs =="
for t in $TESTS; do
    if riscv64-linux-gnu-gcc -static -O2 -o "$BUILD/spacespec_$t" \
            "$HERE/spacespec_$t.c"; then
        echo "   built  spacespec_$t"
    else
        echo "   FAILED to build spacespec_$t" >&2
        exit 2
    fi
done

# Guest printf goes to the gem5 console, not simout (simout only carries
# output() syscall writes), so capture the console and filter to the test's
# own lines.
run_one() {
    local prog="$1" cfg="$2" tag="$3"
    rm -rf "$OUTDIR/$tag"
    timeout "$TIMEOUT" "$GEM5" -d "$OUTDIR/$tag" "$cfg" \
        --cpu-type=O3CPU --caches --cmd="$prog" \
        >"$OUTDIR/$tag.console" 2>&1
    local rc=$?
    if [ $rc -eq 124 ]; then
        # Report on stderr only.  This used to echo the reason to stdout,
        # where the caller compared it against the other run's copy: two
        # timeouts compared equal and were reported as PASS.  An empty
        # stdout makes the caller take its explicit no-result failure path.
        echo "  ${tag}: TIMEOUT after ${TIMEOUT}s" >&2
        return 1
    fi
    grep -E "^spacespec_[a-z]+ done" "$OUTDIR/$tag.console" 2>/dev/null
}

pass=0; fail=0
failed_tests=" "
echo
echo "== output equivalence (baseline vs SpaceSpec) =="
for t in $TESTS; do
    if [ "$t" = shadowhit ]; then
        # The LVP can hide the measured misses; Layer 3 is independent of it.
        base=$(SPACESPEC_LVP=0 run_one "$BUILD/spacespec_$t" "$SE" "${t}_base")
        space=$(SPACESPEC_LVP=0 run_one "$BUILD/spacespec_$t" "$CONFIG" \
                "${t}_space")
    else
        base=$(run_one "$BUILD/spacespec_$t" "$SE" "${t}_base")
        space=$(run_one "$BUILD/spacespec_$t" "$CONFIG" "${t}_space")
    fi

    if [ -z "$base" ] || [ -z "$space" ]; then
        printf "  %-10s FAIL   (no result: crash or timeout)\n" "$t"
        fail=$((fail+1)); failed_tests="$failed_tests$t "
    elif [ "$base" = "$space" ]; then
        printf "  %-10s PASS   %s\n" "$t" "$base"
        pass=$((pass+1))
    else
        printf "  %-10s MISMATCH\n" "$t"
        printf "      baseline: %s\n" "$base"
        printf "      spacespec: %s\n" "$space"
        fail=$((fail+1)); failed_tests="$failed_tests$t "
    fi
done

echo
echo "== self-check (harness verifies its own expectations) =="
for t in $TESTS; do
    # A test that never produced a result line cannot have passed its own
    # self-check.  Grepping only for "FAIL" used to report "ok" for a run that
    # timed out or crashed before printing anything, which is the same
    # vacuous-pass failure as comparing two timeouts.
    case "$failed_tests" in
        *" $t "*)
            printf "  %-10s skipped (already failed above)\n" "$t"
            continue
            ;;
    esac
    if ! grep -qE "^spacespec_[a-z]+ done" "$OUTDIR/${t}_base.console" 2>/dev/null; then
        printf "  %-10s FAIL   (no result line printed)\n" "$t"
        fail=$((fail+1))
    elif grep -q "FAIL" "$OUTDIR/${t}_base.console" 2>/dev/null; then
        printf "  %-10s FAIL   (harness self-check failed)\n" "$t"
        fail=$((fail+1))
    else
        printf "  %-10s ok\n" "$t"
    fi
done

echo
echo "== Layer 3: shadow-hit serve / no-MSHR / merge =="

shadow_stat() {
    local tag=$1 key=$2
    awk -v key="$key" '$1 ~ key {s+=$2} END{print s+0}' \
        "$OUTDIR/$tag/stats.txt" 2>/dev/null
}

noserve=$(SPACESPEC_LVP=0 SPACESPEC_SERVE=0 run_one \
          "$BUILD/spacespec_shadowhit" "$CONFIG" "shadowhit_noserve")
space_line=$(grep -E '^spacespec_shadowhit done' \
             "$OUTDIR/shadowhit_space.console" 2>/dev/null)
noserve_line=$(grep -E '^spacespec_shadowhit done' \
               "$OUTDIR/shadowhit_noserve.console" 2>/dev/null)

if [ -z "$noserve_line" ]; then
    printf "  serve-off      FAIL   (no result)\n"
    fail=$((fail+1))
elif [ "$space_line" != "$noserve_line" ]; then
    printf "  serve-off      MISMATCH\n"
    fail=$((fail+1))
else
    printf "  serve-off      PASS   (output unchanged)\n"
fi

hits=$(shadow_stat shadowhit_space 'shadow_cache\.lookupHits$')
safe=$(shadow_stat shadowhit_space 'shadow_cache\.safeShadowHits$')
unsafe=$(shadow_stat shadowhit_space 'shadow_cache\.unsafeShadowHits$')
refresh=$(shadow_stat shadowhit_space 'shadow_cache\.refreshes$')
mshr=$(shadow_stat shadowhit_space 'dcache\.demandMshrMisses::total$')
refresh_off=$(shadow_stat shadowhit_noserve 'shadow_cache\.refreshes$')
mshr_off=$(shadow_stat shadowhit_noserve 'dcache\.demandMshrMisses::total$')
mshr=${mshr:-0}; mshr_off=${mshr_off:-0}

printf "  serves=%s safe=%s unsafe=%s refresh=%s/%s mshr=%s/%s\n" \
       "$hits" "$safe" "$unsafe" "$refresh" "$refresh_off" "$mshr" "$mshr_off"

if [ "$hits" -ge 3 ]; then
    printf "  shadow_hit     PASS   (%s served)\n" "$hits"
else
    printf "  shadow_hit     FAIL   (expected >=3 serves, got %s)\n" "$hits"
    fail=$((fail+1))
fi
if [ "$unsafe" -eq 0 ]; then
    printf "  safe-only      PASS   (no older/other-thread serves)\n"
else
    printf "  safe-only      FAIL   (%s unsafe serves)\n" "$unsafe"
    fail=$((fail+1))
fi
if [ "$mshr_off" -gt "$mshr" ]; then
    printf "  hit_no_mshr    PASS   (serve avoids %s MSHR misses)\n" \
           "$((mshr_off - mshr))"
else
    printf "  hit_no_mshr    FAIL   (mshr on=%s off=%s)\n" "$mshr" "$mshr_off"
    fail=$((fail+1))
fi
if [ "$refresh_off" -gt "$refresh" ]; then
    printf "  refresh_drop   PASS   (refresh on=%s off=%s)\n" \
           "$refresh" "$refresh_off"
else
    printf "  refresh_drop   FAIL   (refresh on=%s off=%s)\n" \
           "$refresh" "$refresh_off"
    fail=$((fail+1))
fi

echo
echo "== pass=$pass fail=$fail =="
echo "note: the security tests (flop_attack*) are NOT run here -- their"
echo "      pass condition is that the leak disappears, not that output"
echo "      matches. Run those separately once SpaceSpec engages a leak."
echo
echo "logs: $OUTDIR"

[ "$fail" -eq 0 ] || exit 1
exit 0