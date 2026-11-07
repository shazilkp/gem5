#include <stdint.h>
#include <stdio.h>

/*
 * A deliberately unpredictable branch that shadows a load, followed by a
 * second unpredictable branch.  Mispredictions must squash delayed loads;
 * the architectural result must still match the baseline.
 */
#define N (1u << 17)
#define MASK (N - 1)

static uint32_t a[N];

int
main(void)
{
    for (uint32_t i = 0; i < N; i++) {
        a[i] = i * 7 + 3;
    }

    uint32_t seed = 1;
    uint64_t s = 0;
    for (uint32_t i = 0; i < 16384; i++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t idx = (seed >> 16) & MASK;
        uint32_t v;
        if (seed & 0x10000) {
            v = a[idx];
        } else {
            v = a[(idx + N / 2) & MASK];
        }
        if (seed & 0x20000) {
            s += v ^ seed;
        } else {
            s -= v + seed;
        }
    }

    printf("s=%llu\n", (unsigned long long)s);
    return 0;
}
