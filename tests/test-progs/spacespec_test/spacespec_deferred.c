/*
 * SpaceSpec correctness harness: a store that arrives while its line's fill is
 * already in flight.
 *
 * Purpose
 * -------
 * This exists because the first Layer 1 implementation lost stores here, and the
 * original spacespec_store did not catch it.  That test's working set (8 KiB)
 * fits entirely in L1, and its clflush helper is deliberately a no-op, so every
 * store hit a resident block and never exercised the miss path at all.
 *
 * The bug
 * --------
 * A cache MSHR keeps two target lists: the active one, and a *deferred* list for
 * requests that arrived after the fill was already in flight and therefore could
 * not be given the writable block they need.  needsWritable() reports only the
 * active list, so an MSHR whose active targets are all reads but which is also
 * holding a waiting store still reports that it does not need a writable copy.
 *
 * SpaceSpec decides whether to divert a fill by asking exactly that.  Believing
 * it, the fill was diverted into scratch storage instead of being installed.
 * When the response arrived, recvTimingResp() promoted the deferred store and
 * re-sent the MSHR -- which only works if the block is now resident.  It was not,
 * because the fill had been diverted.  The store was applied to scratch storage
 * and dropped.
 *
 * The symptom is silent and delayed: the guest reads back the value it just
 * overwrote.  Nothing crashes, no assert fires, and the fault surfaces far from
 * its cause -- in the first instance as an unrelated segfault thousands of
 * instructions later, in a different test program entirely.
 *
 * What this exercises
 * -------------------
 * Each iteration loads a line and then immediately stores to that same line, so
 * the store is in flight behind the load's fill and becomes a deferred target.
 * The array is far larger than L1 so the lines are genuinely missing, and the
 * final pass reads everything back.
 *
 * Pass condition: simout is byte-for-byte identical with the mechanism on and
 * off.  It is also self-checking, so a wrong answer is reported as a wrong
 * answer rather than only as a mismatch.
 */

#include <stdint.h>
#include <stdio.h>

/*
 * 256 KiB. Comfortably larger than any L1 this will run on, so every line is a
 * real miss and the store really does land on an MSHR rather than a resident
 * block.
 */
#ifndef KB
#define KB 256
#endif

#define NINTS (KB * 1024 / (int)sizeof(int))

/* volatile so the compiler cannot turn the load/store pair into a register
 * operation and remove the memory traffic this test depends on. */
static volatile int buf[NINTS];

int
main(void)
{
    printf("spacespec_deferred start\n");
    printf("  ints=%d\n", NINTS);

    /*
     * The load and the store touch the same line, back to back.  The load
     * misses and allocates an MSHR; the store arrives while that fill is still
     * outstanding, so it cannot be given a writable block and is deferred.
     */
    for (int i = 0; i < NINTS; i++) {
        int v = buf[i];
        buf[i] = v + 1;
    }

    /*
     * Read everything back.  If any deferred store was dropped, the line was
     * never written back to memory, so this re-reads the original zero rather
     * than the incremented value.
     */
    uint64_t bad = 0;
    uint64_t first_bad = 0;
    for (int i = 0; i < NINTS; i++) {
        if (buf[i] != 1) {
            if (bad == 0) {
                first_bad = (uint64_t)i;
            }
            bad++;
        }
    }

    if (bad != 0) {
        printf("FAIL: %llu of %d lines lost their store\n",
               (unsigned long long)bad, NINTS);
    }

    printf("spacespec_deferred done bad=%llu first_bad=%llu expect_bad=0\n",
           (unsigned long long)bad, (unsigned long long)first_bad);

    return 0;
}
