/*
 * Layer 2, I2.4: a split (unaligned) load.
 *
 * An 8-byte access at offset 60 spans two 64-byte lines.  Both lines miss,
 * both fills are captured with the same owner sequence number, and both must
 * be installed when the owning load commits.  Before the fix, promote()
 * released only the first entry and the second was swept away, so one line
 * never reached the cache.
 *
 * The value check catches data corruption; the assertion in
 * ShadowCache::promote() catches a regression where the owner is left with an
 * entry after promotion.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LINE 64

static uint8_t buf[4 * LINE] __attribute__((aligned(64)));
/* volatile: otherwise the compiler deletes the sweep because the buffer is
 * never read, and the measured load hits instead of missing. */
static volatile uint8_t flush_buf[128 * 1024];

int main(void)
{
    for (int i = 0; i < (int)sizeof(buf); i++)
        buf[i] = (uint8_t)(i * 7 + 3);

    /* Reference value.  This reads both lines, so it must happen before the
     * flush, otherwise the measured load below would hit. */
    uint64_t want = 0;
    memcpy(&want, buf + 60, sizeof(want));

    /* Evict buf from L1 so the unaligned load below misses both lines. */
    for (int i = 0; i < (int)sizeof(flush_buf); i += LINE)
        flush_buf[i] = (uint8_t)i;

    /*
     * Force a single 8-byte load at an unaligned address.  The compiler will
     * otherwise lower this to two 4-byte loads, which never creates the
     * SplitDataRequest this test is about.
     */
    uint64_t got = 0;
    asm volatile("ld %0, 0(%1)" : "=r"(got) : "r"(buf + 60) : "memory");

    printf("spacespec_split start want=%016llx got=%016llx\n",
           (unsigned long long)want, (unsigned long long)got);

    if (want != got) {
        printf("spacespec_split FAIL\n");
        return 1;
    }

    printf("spacespec_split done checksum=%016llx\n",
           (unsigned long long)got);
    return 0;
}
