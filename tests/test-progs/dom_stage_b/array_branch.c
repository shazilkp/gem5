#include <stdint.h>
#include <stdio.h>

/*
 * A branchy array kernel: loads, stores, and data-dependent branches.  Used
 * as an architectural-equality regression for Delay-on-Miss.
 */
#define N 65536

static uint32_t a[N];

int
main(void)
{
    for (uint32_t i = 0; i < N; i++) {
        a[i] = (i * 2654435761u) ^ (i << 3);
    }

    uint64_t s = 0;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t v = a[i];
        if (v & 1) {
            s += v;
        } else {
            s ^= v;
        }
        a[i] = (uint32_t)s;
    }

    printf("s=%llu\n", (unsigned long long)s);
    return 0;
}
