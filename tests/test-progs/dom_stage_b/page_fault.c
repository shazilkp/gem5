#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * B5 page-fault regression: map, touch/evaluate, unmap, remap, touch again.
 * The syscall and page-fault paths must invalidate the provider; run with
 * GEM5_PROV_CHECK=1 so every provider hit is compared with memory.
 */
#define SZ (1u << 20)

int
main(void)
{
    uint8_t *p = mmap(NULL, SZ, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return 1;
    }

    uint64_t s = 0;
    for (uint32_t i = 0; i < SZ; i += 4096) {
        p[i] = (uint8_t)(i >> 12);
    }
    for (uint32_t i = 0; i < SZ; i += 4096) {
        s += p[i];
    }

    munmap(p, SZ);

    uint8_t *q = mmap(NULL, SZ, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED) {
        return 1;
    }

    for (uint32_t i = 0; i < SZ; i += 4096) {
        q[i] = (uint8_t)((i >> 12) + 1);
    }
    for (uint32_t i = 0; i < SZ; i += 4096) {
        s += q[i];
    }

    printf("s=%llu\n", (unsigned long long)s);
    return 0;
}
