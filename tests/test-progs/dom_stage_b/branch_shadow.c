#include <stdint.h>
#include <stdio.h>

/*
 * B0/B1: a load after a data-dependent branch that misses in L1.
 *
 *   a[0] is a miss; the branch depends on it; the a[0x4000] load is in a
 *   different line and misses. Under Delay-on-Miss the branch casts a
 *   C-shadow, so the second load's miss must be dropped and re-issued only
 *   after the branch is non-speculative. The architectural result must be
 *   identical to the baseline.
 */
#define N 65536
#define FAR (N / 2)

static uint32_t a[N];
static volatile uint32_t sink;

int main(void)
{
    uint32_t x = a[0];
    uint32_t y;

    if (x == 0) {
        y = a[FAR];
    } else {
        y = a[FAR + 1];
    }

    sink = y;
    printf("x=%u y=%u\n", x, y);
    return 0;
}
