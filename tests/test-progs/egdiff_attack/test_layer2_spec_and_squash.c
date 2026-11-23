/*
 * Layer 2 verification (part 1): speculative GVQ lifetime, normal dispatch
 * prediction, LQ verification/squash, and the 1024-instruction lastMisp
 * blackout on a clean (branch-free) global-stride loop.
 *
 * Phase 1  train: B[i] = A[i] + 40, with one intermediate load between A and
 *          B, so L_target's dynamic distance is 2.
 * Phase 2  exactly one iteration uses B[i] = A[i] + 99  ->  exactly one value
 *          misprediction, one squash, ctr reset, distance poll 2 -> 1.
 * Phase 3  resume the correct stride: after the blackout, training re-locks at
 *          d = 2, confidence re-saturates, and predictions resume.
 *
 * The checksum is recomputed from memory, so a mis-forwarded value would show
 * up as a mismatch in the final "OK" line.
 */

#include <stdint.h>
#include <stdio.h>

#define TRAIN   300
#define RECOVER 700
#define N       (TRAIN + 1 + RECOVER)

static volatile uint64_t A[N];
static volatile uint64_t B[N];
static volatile uint64_t C[N];

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
        B[i] = a + 40;
    }

    /* Expected sum *before* the induced change: every value follows the +40
     * stride, so any table entry this loop trains also matches and it cannot
     * introduce a misprediction of its own. */
    uint64_t expect = 0;
    for (int i = 0; i < N; i++)
        expect += A[i] + C[i] + B[i];

    B[TRAIN] = A[TRAIN] + 99;   /* the single induced misprediction */
    expect += 99 - 40;          /* account for the changed element */

    uint64_t sink = 0;
    for (int i = 0; i < N; i++) {
        uint64_t b = A[i];   /* L_base   */
        uint64_t m = C[i];   /* intermed */
        uint64_t t = B[i];   /* L_target */
        sink += b + m + t;
    }

    printf("layer2 sink=%llu expect=%llu %s\n",
           (unsigned long long)sink, (unsigned long long)expect,
           (sink == expect) ? "OK" : "MISMATCH");
    return sink == expect ? 0 : 1;
}
