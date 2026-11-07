#include <stdint.h>
#include <stdio.h>

/*
 * Stage A smoke microbenchmark.
 *
 * affine: written once, then read twice -> the committed-value oracle can
 *         serve both read passes from the stores.
 * zeros:  BSS, never written -> the first pass populates the oracle from
 *         the committed loads; the second pass is serveable.
 *
 * Both arrays exceed the 64 KiB L1-D so the reads actually miss.
 */
#define N 65536

static uint32_t affine[N];
static uint32_t zeros[N];

int main(void)
{
    for (int i = 0; i < N; i++) {
        affine[i] = 100u + 3u * (uint32_t)i;
    }

    uint64_t sum = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < N; i++) {
            sum += affine[i];
        }
        for (int i = 0; i < N; i++) {
            sum += zeros[i];
        }
    }

    printf("affine=%p zeros=%p N=%d sum=%llu\n",
           (void *)affine, (void *)zeros, N, (unsigned long long)sum);
    return 0;
}
