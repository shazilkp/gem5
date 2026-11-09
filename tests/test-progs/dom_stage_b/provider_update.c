#include <stdint.h>
#include <stdio.h>

/*
 * B5: two committed stores to the same word.  The second store must update
 * the provider entry, so a later shadowed L1-miss load observes the new
 * value, not the first one.
 */
#define STREAM 32768
#define COLD 65536

static uint32_t stream[STREAM];
static uint32_t cold[COLD];
static volatile uint32_t x;

int
main(void)
{
    x = 1;
    x = 99;

    for (uint32_t i = 0; i < STREAM; i++) {
        stream[i] = i;
    }

    uint64_t s = 0;
    for (uint32_t i = 0; i < STREAM; i++) {
        s += stream[i];
    }

    uint32_t c = cold[0];
    uint32_t y;
    if (c == 0) {
        y = x;
    } else {
        y = x + 1;
    }

    printf("x=%u y=%u s=%llu\n", x, y, (unsigned long long)s);
    return 0;
}
