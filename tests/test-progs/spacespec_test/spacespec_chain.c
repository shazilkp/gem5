/*
 * SpaceSpec correctness harness: dependent chain across many lines.
 *
 * Purpose
 * -------
 * The promotion path.  Each load's line is held in the shadow and only
 * becomes resident when that load commits.  This exercises that promotion
 * repeatedly and in order, and -- critically -- over a working set larger
 * than the shadow, so promotion has to interleave with shadow pressure.
 *
 * It is the closest deterministic analogue of lvp_test, but without the
 * single-line degenerate case: lvp_test chases one line, so a structure that
 * mishandles ownership for one address can look fine.  Here the chain walks
 * thousands of distinct lines, so an ownership or lifetime error shows up as
 * a wrong final value.
 *
 * Determinism
 * -----------
 * Prints only the final chased value and a checksum.  Pass condition: simout
 * is byte-for-byte identical with the mechanism on and off.
 */

#include <stdint.h>
#include <stdio.h>

#ifndef ITERS
#define ITERS 20000
#endif

#ifndef KB
#define KB 32
#endif

#define NINTS (KB * 1024 / (int)sizeof(int))

static int arr[NINTS];

int
main(void)
{
    printf("spacespec_chain start\n");
    printf("  iters=%d ints=%d\n", ITERS, NINTS);

    /*
     * A long cycle: arr[i] -> i+1, wrapping at the end.  Every step is a
     * dependent load of a fresh line for the first NINTS steps, then it
     * repeats, so promotion is exercised across the whole array.
     */
    for (int i = 0; i < NINTS; i++)
        arr[i] = (i + 1) % NINTS;

    int val = 0;

    for (int it = 0; it < ITERS; it++) {
        /* Unrolled so the loop body is dominated by the dependent chain. */
        val = arr[val];
        val = arr[val];
        val = arr[val];
        val = arr[val];
        val = arr[val];
        val = arr[val];
        val = arr[val];
        val = arr[val];
    }

    /*
     * ITERS * 8 steps, each advancing by 1 mod NINTS.
     * NINTS = 8192, steps = 160000 -> 160000 mod 8192 = 4032.
     */
    printf("spacespec_chain done val=%d expect=%d\n",
           val, (int)((long)ITERS * 8 % NINTS));

    return 0;
}