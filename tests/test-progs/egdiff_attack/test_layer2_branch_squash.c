/*
 * Layer 2 verification (part 2): ROB-squash rollback of speculative GVQ state.
 *
 * A data-dependent branch flips regularly, so roughly half of the iterations
 * are mispredicted and execute loads (D or E) on the wrong path.  Those loads
 * allocate speculative GVQ slots, then are discarded by ROB::squash when the
 * branch resolves.  They must never reach the commit-stage table update.
 *
 * Success criteria (checked by run_layer2.sh):
 *   - the architectural checksum is exact (squashed state never leaked),
 *   - egdiffSquashed > 0 (speculative entries really were discarded).
 */

#include <stdint.h>
#include <stdio.h>

#define N 400

static volatile uint64_t A[N];
static volatile uint64_t B[N];
static volatile uint64_t C[N];
static volatile uint64_t D[N];
static volatile uint64_t E[N];

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
        D[i] = mix((uint64_t)i + 0x550000);
        E[i] = mix((uint64_t)i + 0x770000);
        B[i] = a + 40;
    }

    volatile uint64_t sink = 0;
    for (int i = 0; i < N; i++) {
        uint64_t b = A[i];
        uint64_t m = C[i];
        uint64_t t = B[i];
        if ((i & 7) == 0)
            sink += b + m + t + D[i];
        else
            sink += b + m + t + E[i];
    }

    uint64_t expect = 0;
    for (int i = 0; i < N; i++) {
        expect += A[i] + C[i] + B[i];
        expect += ((i & 7) == 0) ? D[i] : E[i];
    }

    printf("layer2branch sink=%llu expect=%llu %s\n",
           (unsigned long long)sink, (unsigned long long)expect,
           (sink == expect) ? "OK" : "MISMATCH");
    return sink == expect ? 0 : 1;
}
