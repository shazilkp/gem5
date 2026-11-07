#include <stdint.h>
#include <stdio.h>

/*
 * A load misses (a[0]); a dependent load to the same line executes after the
 * fill but before the first load commits.  Under DoM the second load hits a
 * line that the first load installed speculatively, so it is a delayed miss
 * (L kind=1) and must be counted separately from a committed L1 hit.
 *
 * The older loop fills the ROB so the first load is not at the head when its
 * data returns; that keeps the "installed but not committed" window open.
 */
#define N 65536
static uint32_t a[N];
static volatile uint32_t sink;

int main(void)
{
    uint32_t x = 0;
    for (uint32_t k = 0; k < 1000; k++) {
        x += k;
    }

    uint32_t i = a[0];
    uint32_t y = a[i];

    sink = x + y;
    printf("i=%u x=%u y=%u\n", i, x, y);
    return 0;
}
