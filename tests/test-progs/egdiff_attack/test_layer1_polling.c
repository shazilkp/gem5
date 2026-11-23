/*
 * Layer 1 verification: commit-stage Distance Polling (Algorithm 1).
 *
 * Executes a loop with three volatile loads per iteration in program order:
 *
 *     b = A[i];   // L_base     (0x106d8)
 *     m = C[i];   // intermediate (0x106de)
 *     t = B[i];   // L_target   (0x106e2)   dynamic load distance d = 2
 *
 * The arrays are filled with pseudorandom values so that B is *not* a
 * self-strided function of the loop index: the ONLY load distance at which
 * B[i] minus the value of the load `d` positions earlier is constant is d = 2
 * (where that load is A[i] and the difference is exactly STRIDE = 40).
 *
 * That matters: Algorithm 1 locks onto the first distance whose difference is
 * constant across iterations.  An index-linear test such as B[i] = 7*i + 41 is
 * constant at every distance that is a multiple of the 3 loads per iteration,
 * so the entry locks onto a self-distance instead of the intended d = 2.
 *
 * From iteration STEADY onwards B[i] = A[i] + BAD_STRIDE, so the first such
 * commit must reset ctr -> 0, u -> 0 and poll distance 2 -> 1.
 *
 * Verification is done from the --debug-flags=EgDiff trace and stats.txt by
 * tests/test-progs/egdiff_attack/run_layer1.sh.
 */

#include <stdint.h>
#include <stdio.h>

#define N 600
#define STEADY 400
#define STRIDE 40
#define BAD_STRIDE 99

static volatile uint64_t A[N];
static volatile uint64_t B[N];
static volatile uint64_t C[N];

/* splitmix64, just to get deterministic, non-linear array contents */
static uint64_t
mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

int main(void)
{
    for (int i = 0; i < N; i++) {
        uint64_t a = mix((uint64_t)i + 0x1000);
        A[i] = a;
        C[i] = mix((uint64_t)i + 0x900000);
        B[i] = a + (i < STEADY ? STRIDE : BAD_STRIDE);
    }

    uint64_t sink = 0;

    for (int i = 0; i < N; i++) {
        uint64_t b = A[i];   /* L_base   */
        uint64_t m = C[i];   /* intermed */
        uint64_t t = B[i];   /* L_target */
        sink += b + m + t;
    }

    printf("layer1 done sink=%llu\n", (unsigned long long)sink);
    return 0;
}
