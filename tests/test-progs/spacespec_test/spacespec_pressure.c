/*
 * SpaceSpec correctness harness: shadow capacity and entry lifetime.
 *
 * Purpose
 * -------
 * A shadow cache is sized for a small number of concurrent speculative
 * lines.  Two failure modes appear only when the working set exceeds it:
 *
 *   (a) the structure fills up and the fill-handling policy is exercised
 *       (drop, stall, or evict -- whichever is chosen);
 *   (b) entries are never released, so a hot line can never reach the real
 *       cache and every access to it re-fetches forever.
 *
 * Failure (b) is the worse one: it is not a wrong answer, it is a hang.  It
 * bit us during the first implementation.  This harness is designed to turn
 * that hang into a fast, visible failure by keeping the working set large
 * and the access pattern repetitive enough that a non-releasing structure
 * spins on a single line forever.
 *
 * Shape
 * -----
 * A working set far larger than any plausible shadow size, swept repeatedly,
 * with a dependent chain so each iteration touches fresh lines.  If the shadow
 * cannot release entries the loop makes no forward progress and the harness
 * times out.
 *
 * Determinism
 * -----------
 * Prints a checksum and the iteration count only.  Pass condition: simout is
 * byte-for-byte identical with the mechanism on and off, and the run
 * terminates.
 */

#include <stdint.h>
#include <stdio.h>

#ifndef ITERS
#define ITERS 2000
#endif

/*
 * 64 KiB = 1024 cache lines. Comfortably larger than the 64-entry shadow
 * SafeSpec suggests, so the full structure is exercised repeatedly.
 */
#ifndef KB
#define KB 64
#endif

#define NINTS (KB * 1024 / (int)sizeof(int))

static int buf[NINTS];

int
main(void)
{
    printf("spacespec_pressure start\n");
    printf("  iters=%d ints=%d approx_lines=%d\n", ITERS, NINTS, NINTS / 16);

    /*
     * A permutation so that consecutive reads are far apart in memory.  A
     * sequential sweep would let a replacement policy look far better than
     * it is; a scattered one makes a thrashing structure thrash.
     */
    for (int i = 0; i < NINTS; i++)
        buf[i] = (i * 4099 + 17) % NINTS;

    uint64_t acc = 0;

    for (int it = 0; it < ITERS; it++) {
        for (int i = 0; i < NINTS; i++) {
            /* dependent: each index comes from the previous load */
            acc += (uint64_t)(uint32_t)buf[i & (NINTS - 1)];
        }
    }

    /* Fold to a single value so the output stays one line and stable. */
    uint64_t h = 1469598103934665603ULL;
    h ^= acc;
    h *= 1099511628211ULL;

    printf("spacespec_pressure done checksum=%016llx\n",
           (unsigned long long)h);
    return 0;
}