#include <stdint.h>
#include <stdio.h>

/*
 * A data-dependent branch is mispredicted; the wrong-path load must appear
 * as a squashed L (X) and must not populate the committed-value table.
 */
#define N 65536
static uint32_t a[N];
static volatile uint32_t sink;

int main(void)
{
    uint32_t x = a[0];      /* miss, feeds the branch */

    uint32_t y;
    if (x == 0) {
        y = a[16384];       /* wrong path if the branch predicts not-taken */
    } else {
        y = a[32768];
    }

    sink = y;
    printf("x=%u y=%u\n", x, y);
    return 0;
}
