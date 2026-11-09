#include <stdint.h>
#include <stdio.h>

/*
 * Serial pointer chase over a large array.  Every load misses and depends on
 * the previous load, so E-shadows form a long chain.  A self-shadow or a
 * missed release will hang this program; the runner has a hard timeout.
 */
#define N (1u << 18)
#define STEPS 20000

static uint32_t a[N];

int
main(void)
{
    for (uint32_t i = 0; i < N; i++) {
        a[i] = (uint32_t)(((uint64_t)i * 2654435761u) & (N - 1));
    }

    uint32_t idx = 0;
    uint64_t s = 0;
    for (uint32_t i = 0; i < STEPS; i++) {
        idx = a[idx];
        s += idx;
    }

    printf("s=%llu idx=%u\n", (unsigned long long)s, idx);
    return 0;
}
