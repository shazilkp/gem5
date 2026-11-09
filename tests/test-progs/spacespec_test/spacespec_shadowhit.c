/*
 * Layer 3: shadow-hit serve, no-MSHR, and merge.
 *
 * Shape
 * -----
 * Sixteen older misses to distinct lines occupy the ROB head, so the loads
 * below cannot commit while their fills are in flight.  The measured load A
 * then misses and captures its line in the shadow.  B, C and D depend on A
 * (and on each other) and touch the same line; they issue after A's fill, but
 * before A can commit, so the line is still held.
 *
 * What it proves
 * --------------
 * - B/C/D are served from the held line (lookupHits >= 3).
 * - They are younger than the owner and in the same thread, so
 *   unsafeShadowHits must be zero.
 * - The served loads do not open a second memory request: mshrMisses and
 *   refreshes are lower than the SPACESPEC_SERVE=0 run, where the same
 *   consumers have to re-fetch.
 *
 * The program prints deterministic values only, so the base/SpaceSpec output
 * is byte-identical.
 */

#include <stdint.h>
#include <stdio.h>

#define LINE_INTS 16
#define NBLOCK 16

static volatile int line[LINE_INTS] __attribute__((aligned(64)));
static volatile int blockers[NBLOCK][LINE_INTS] __attribute__((aligned(64)));

/* 256 KiB: larger than any L1 used by the harness, so line and blockers are
 * evicted before the measured loads. */
static volatile int flush_buf[65536];

int
main(void)
{
    line[0] = 1;
    line[1] = 42;
    line[2] = 7;
    line[3] = 11;

    for (int i = 0; i < NBLOCK; i++) {
        for (int j = 0; j < LINE_INTS; j++) {
            blockers[i][j] = i * 100 + j;
        }
    }

    for (unsigned i = 0; i < sizeof(flush_buf) / sizeof(flush_buf[0]); i++) {
        flush_buf[i] = (int)i;
    }

    /* Older misses: keep the commit head busy so A cannot commit before the
     * dependent consumers issue. */
    int sink = 0;
    for (int i = 0; i < NBLOCK; i++) {
        sink += blockers[i][0];
    }

    /* A misses and is captured. */
    int a = line[0];

    /* B/C/D are strictly younger, same thread, same line.  Each address
     * depends on the previous load, so they issue after A's fill. */
    int b = line[a & 3];
    int c = line[b & 3];
    int d = line[c & 3];

    printf("spacespec_shadowhit done sink=%d a=%d b=%d c=%d d=%d\n",
           sink, a, b, c, d);
    return 0;
}
