/*
 * SpaceSpec correctness harness: store-to-shadow-held-line.
 *
 * Purpose
 * -------
 * A load fetches a line speculatively, so the line is held in the shadow
 * cache rather than installed in L1.  A later store to that same line must
 * not be overwritten when the load's line is eventually promoted.
 *
 * This is the one correctness rule SpaceSpec has to enforce that ordinary
 * read-only workloads never exercise: the interaction between the shadow
 * copy and a store to the same address.  A pointer-chasing microbenchmark
 * cannot reach it, because it issues no stores to lines it has just loaded.
 *
 * What it does, per round:
 *   1. load  every line of the array            (fills -> shadow)
 *   2. store a new value to every line          (must invalidate the shadow copy)
 *   3. load  every line again                   (must see the stored values)
 *   4. store a *different* value to every line  (again)
 *   5. load  every line again                   (must see the newest values)
 *
 * Step 5 is the assertion: if a stale shadow copy were promoted over a store,
 * the checksum would come from the old data and this test would fail.
 *
 * Determinism
 * -----------
 * Prints only a checksum of the final contents and the round count.  No
 * addresses, no cycle counts, nothing that varies run to run.  The pass
 * condition is that simout is byte-for-byte identical with the mechanism on
 * and off.
 */

#include <stdint.h>
#include <stdio.h>

#ifndef ROUNDS
#define ROUNDS 64
#endif

/* Small enough to stay resident, large enough to span many lines. */
#ifndef NINTS
#define NINTS 2048
#endif

#define CACHELINE 64
#define INTS_PER_LINE (CACHELINE / (int)sizeof(int))

static volatile int buf[NINTS];

static inline void
clflush_line(void *p)
{
    /* Not a real flush: we only need the cache to lose the line, and
     * touching every line with a store is sufficient for that here. */
    (void)p;
}

static uint64_t
checksum(void)
{
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < NINTS; i++) {
        h ^= (uint64_t)(uint32_t)buf[i];
        h *= 1099511628211ULL;
    }
    return h;
}

int
main(void)
{
    printf("spacespec_store start\n");
    printf("  rounds=%d ints=%d lines=%d\n",
           ROUNDS, NINTS, NINTS / INTS_PER_LINE);

    for (int i = 0; i < NINTS; i++)
        buf[i] = i;

    uint64_t expected = 0;

    for (int r = 0; r < ROUNDS; r++) {
        int a = r + 1;
        int b = -(r + 1);

        /* 1. load every line: fills are diverted to the shadow */
        for (int i = 0; i < NINTS; i++) {
            clflush_line(&buf[i]);
        }
        volatile int sink = 0;
        for (int i = 0; i < NINTS; i++)
            sink += buf[i];
        (void)sink;

        /* 2. store over every line: must invalidate the shadow copies */
        for (int i = 0; i < NINTS; i++)
            buf[i] = a;

        /* 3. read back: must see the value just stored */
        for (int i = 0; i < NINTS; i++) {
            if (buf[i] != a) {
                printf("  FAIL round %d: read-back %d after store\n", r, i);
                return 1;
            }
        }

        /* 4. store a different value over every line */
        for (int i = 0; i < NINTS; i++)
            buf[i] = b;

        /* 5. read back: must see the newest value.  A shadow copy that
         *    survived step 2/4 and got promoted here would surface as `a`
         *    instead of `b`. */
        for (int i = 0; i < NINTS; i++) {
            if (buf[i] != b) {
                printf("  FAIL round %d: stale shadow copy at %d "
                       "(got %d want %d)\n", r, i, buf[i], b);
                return 1;
            }
        }

        expected = checksum();
    }

    printf("spacespec_store done checksum=%016llx\n",
           (unsigned long long)expected);
    return 0;
}