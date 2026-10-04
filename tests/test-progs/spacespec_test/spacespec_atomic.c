/*
 * SpaceSpec correctness harness: atomics must not be perturbed.
 *
 * Purpose
 * -------
 * An LL/SC pair needs a stable, already-resident block.  If a mechanism
 * invalidates a block underneath a store-conditional, the SC fails; if it
 * invalidates it again on the retry, the pair never succeeds and the program
 * livelocks.
 *
 * This is not hypothetical -- it was exactly how the first SpaceSpec
 * implementation hung, with the CPU pegged at 100% and no progress:
 *
 *     access for StoreCondReq  [65630:65633] hit
 *     ... block dropped so the store can re-fetch ...
 *     access for LoadLockedReq [65630:65633] miss   -> repeats indefinitely
 *
 * Note this is a *liveness* failure, not a wrong-answer failure, so it does
 * not show up in a byte-for-byte output diff.  It shows up as a hang.  The
 * pass condition is therefore twofold:
 *   - the program terminates (the runner enforces a timeout), and
 *   - the counters are exactly as predicted.
 *
 * Uses __atomic builtins rather than hand-written asm: on RISC-V these lower
 * to genuine lr/sc pairs, which is what we need to exercise, and they are
 * correct by construction rather than by inspection.
 *
 * Determinism
 * -----------
 * Every counter is fully determined by the sequential semantics, so the
 * totals below are exact.  Pass condition: identical output with the
 * mechanism on and off, and the run terminates.
 */

#include <stdint.h>
#include <stdio.h>

#ifndef ITERS
#define ITERS 50000
#endif

/* Small set of cache lines, hammered hard, to force contention. */
#ifndef SLOTS
#define SLOTS 64
#endif

static volatile int counter[SLOTS];
static volatile long payload[SLOTS];
static volatile int cas_flag[SLOTS];

int
main(void)
{
    printf("spacespec_atomic start\n");
    printf("  iters=%d slots=%d\n", ITERS, SLOTS);

    for (int i = 0; i < SLOTS; i++) {
        counter[i] = 0;
        payload[i] = 0;
        cas_flag[i] = 0;
    }

    /*
     * Phase 1: contended fetch_add over a few lines.
     * Exactly ITERS increments, so counter_total must be exactly ITERS.
     */
    for (int it = 0; it < ITERS; it++) {
        int s = it % SLOTS;
        __atomic_fetch_add(&counter[s], 1, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&payload[s], 2, __ATOMIC_SEQ_CST);
    }

    /*
     * Phase 2: compare-and-swap on a clean flag array.
     * Each slot flips 0 -> 1 exactly once regardless of how many attempts
     * lose, so exactly SLOTS CAS operations can possibly succeed.  This is
     * the property that a livelocking implementation would violate: if a
     * block is repeatedly invalidated under the pair, it never wins.
     */
    int cas_success = 0;
    for (int it = 0; it < ITERS; it++) {
        int s = it % SLOTS;
        int expected = 0;
        if (__atomic_compare_exchange_n(&cas_flag[s], &expected, 1, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            cas_success++;
        }
    }

    uint64_t counter_total = 0;
    uint64_t payload_total = 0;
    int cas_flag_total = 0;
    for (int i = 0; i < SLOTS; i++) {
        counter_total += (uint64_t)counter[i];
        payload_total += (uint64_t)payload[i];
        cas_flag_total += cas_flag[i];
    }

    /* Exact totals: a lost update or a stuck loop becomes visible here. */
    if (counter_total != (uint64_t)ITERS) {
        printf("  FAIL: counter_total=%llu expected %d\n",
               (unsigned long long)counter_total, ITERS);
        return 1;
    }
    if (payload_total != (uint64_t)ITERS * 2) {
        printf("  FAIL: payload_total=%llu expected %d\n",
               (unsigned long long)payload_total, ITERS * 2);
        return 1;
    }
    if (cas_flag_total != SLOTS || cas_success != SLOTS) {
        printf("  FAIL: cas_flag_total=%d cas_success=%d expected %d\n",
               cas_flag_total, cas_success, SLOTS);
        return 1;
    }

    printf("spacespec_atomic done counter_total=%llu payload_total=%llu "
           "cas_success=%d\n",
           (unsigned long long)counter_total,
           (unsigned long long)payload_total,
           cas_success);
    return 0;
}