#include <stdint.h>
#include <stdio.h>

/*
 * B5: a committed store populates the value provider.  The line is then
 * evicted from L1 by a stream, and a later load under an unresolved branch
 * misses L1.  The provider must supply the committed value without a memory
 * access, and the architectural result must match the baseline.
 */
#define STREAM 32768
#define COLD 65536

static uint32_t stream[STREAM];
static uint32_t cold[COLD];
static uint32_t x;

int
main(void)
{
    x = 42;

    for (uint32_t i = 0; i < STREAM; i++) {
        stream[i] = i;
    }

    uint64_t s = 0;
    for (uint32_t i = 0; i < STREAM; i++) {
        s += stream[i];
    }

    uint32_t c = cold[0];   /* cold miss; the branch waits on it */
    uint32_t y;
    if (c == 0) {
        y = x;              /* L1 miss, provider hit */
    } else {
        y = x + 1;
    }

    printf("x=%u y=%u s=%llu\n", x, y, (unsigned long long)s);
    return 0;
}
