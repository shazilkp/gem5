#include <stdint.h>
#include <stdio.h>

/*
 * B2: a load that is shadowed by an unresolved branch and hits in L1.  The
 * branch depends on a cold miss, so the shadow window is long enough for the
 * hit load to issue under it.  The cache must return the data but defer the
 * replacement-policy update until the branch is verified.  The deferred and
 * released cache counters prove the path ran; the output must match.
 */
#define N 65536

static uint32_t a[N];
static uint32_t b[16];

int
main(void)
{
    b[0] = 7;
    b[1] = 7;
    b[2] = 9;
    b[3] = 11;

    uint32_t warm = b[0];   /* hit: the line was allocated by the stores */
    uint32_t x = a[0];      /* cold miss; the branch waits on this */
    uint32_t y;

    if (x == 0) {           /* unresolved while b[y-index] issues */
        y = b[1];           /* shadowed L1 hit */
    } else {
        y = b[2];           /* shadowed L1 hit */
    }

    printf("warm=%u x=%u y=%u\n", warm, x, y);
    return 0;
}
