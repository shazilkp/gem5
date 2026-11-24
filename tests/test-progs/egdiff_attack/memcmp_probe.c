/*
 * One-call probe for the architectural-vs-dispatch distance check.
 *
 * Calls the real libc memcmp(), then executes a marker load as the *first*
 * load after the call returns (the compare result is stored, not reloaded
 * before the marker).  With argv "equal" the buffers compare equal; with
 * "differ" the first byte differs, so memcmp exits early.
 *
 * Build:
 *   riscv64-linux-gnu-gcc -static -O2 -fno-pic -no-pie -fno-builtin-memcmp \
 *       -o memcmp_probe memcmp_probe.c
 * Then measure with measure_dispatch_d.py against a qemu -singlestep trace and
 * a gem5 --debug-flags=EgDiff trace.
 */
#include <stdint.h>
#include <string.h>

static const uint8_t secret[32] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
    0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
    0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0,
};

static volatile uint64_t sink;
static volatile int g_result;
static volatile uint64_t marker_buf[64];

__attribute__((noinline, noclone, noipa))
static int
target_memcmp(const uint8_t *guess)
{
    return memcmp(guess, secret, 32);
}

__attribute__((noinline, noclone, noipa))
static uint64_t
atk_load(volatile uint64_t *base, uint64_t idx)
{
    return base[idx & 63];
}

int
main(int argc, char **argv)
{
    uint8_t guess[32];
    for (int i = 0; i < 32; i++)
        guess[i] = secret[i];
    if (argc > 1 && argv[1][0] == 'd')
        guess[0] ^= 0x01;          /* early exit after the first byte */

    /* Store the result (no load) so the marker stays the first load after the
     * call; consume the result only after the marker. */
    g_result = target_memcmp(guess);
    sink += atk_load(marker_buf, 0);
    sink += (uint64_t)g_result;
    return 0;
}
