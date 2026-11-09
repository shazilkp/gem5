#include <stdint.h>
#include <stdio.h>

/*
 * B3: page-strided accesses thrash the DTLB.  A load whose translation is
 * still pending casts an E-shadow; younger loads behind it are delayed until
 * the translation completes.  The output must match the baseline.
 */
#define PAGES 4096
#define STRIDE 1024   /* one page of uint32_t */

static uint32_t a[PAGES * STRIDE];

int
main(void)
{
    for (uint32_t i = 0; i < PAGES; i++) {
        a[i * STRIDE] = i;
    }

    uint64_t s = 0;
    for (uint32_t i = 0; i < PAGES; i++) {
        s += a[i * STRIDE];
    }

    printf("s=%llu\n", (unsigned long long)s);
    return 0;
}
