#include <stdint.h>
#include <stdio.h>

/*
 * A 4 KiB array summed 1000 times.  Everything hits in L1 after warm-up, so
 * Delay-on-Miss must not delay the loads: the DoM IPC must stay close to the
 * baseline IPC.  This catches a bug that treats hits as misses.
 */
#define N 1024
#define ROUNDS 1000

static uint32_t a[N];

int
main(void)
{
    for (uint32_t i = 0; i < N; i++) {
        a[i] = i;
    }

    uint64_t s = 0;
    for (uint32_t r = 0; r < ROUNDS; r++) {
        for (uint32_t i = 0; i < N; i++) {
            if (a[i] & 1) {
                s += a[i];
            } else {
                s ^= a[i];
            }
        }
    }

    printf("s=%llu\n", (unsigned long long)s);
    return 0;
}
