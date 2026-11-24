/*
 * Shared victim/oracle translation unit for the EgDiff conformance experiments.
 *
 * Ownership split (this is the point of the file):
 *
 *   - The target value (Mode 1/2) and the SipHash key (Layer 5) are `static`
 *     objects here.  No other translation unit can name them, so the attacker
 *     code in poc_v1 / poc_v2 / siphash_attack has no architectural reference
 *     to the value it is supposed to obtain.
 *   - The only code that reads the value is the helper (which is what an
 *     in-process privileged component would do) and the post-hoc oracle report,
 *     which plays the role of the experimenter checking the answer.  The oracle
 *     runs after the measurement and is outside the attacker's code path.
 *
 * This mirrors the usual same-process threat model (e.g. a script that cannot
 * read engine-internal state): the claim is that the attacker's instruction
 * stream never loads the value, and the only consumer of it is the helper.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Target value (Mode 1 / Mode 2)                                      */
/* ------------------------------------------------------------------ */
#ifndef SECRET_VAL
#define SECRET_VAL 0x1122334455667788ULL
#endif

static volatile uint64_t secret_value_storage = SECRET_VAL;

/* Branchless helper: reads the value, uses it in registers only, and scrubs
 * the register before returning.  Reads nothing else. */
__attribute__((noinline, noclone, noipa))
uint64_t
victim_value(uint64_t token)
{
    uint64_t s = secret_value_storage;
    uint64_t ok = (s == token);
    asm volatile("" : "+r"(s));
    s = 0;
    asm volatile("" : "+r"(s));
    return ok;
}

/* Same helper, plus a serialising fence before returning (Mode 2). */
__attribute__((noinline, noclone, noipa))
uint64_t
victim_value_fenced(uint64_t token)
{
    uint64_t s = secret_value_storage;
    uint64_t ok = (s == token);
    asm volatile("" : "+r"(s));
    s = 0;
    asm volatile("" : "+r"(s));
    asm volatile("fence.i" ::: "memory");
    return ok;
}

/* ------------------------------------------------------------------ */
/* SipHash-2-4 (canonical majek/csiphash reference) and its helper     */
/* ------------------------------------------------------------------ */
#define ROTATE(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define HALF_ROUND(a, b, c, d, s, t)                                           \
    a += b;                                                                    \
    c += d;                                                                    \
    b = ROTATE(b, s) ^ a;                                                      \
    d = ROTATE(d, t) ^ c;                                                      \
    a = ROTATE(a, 32);

#define DOUBLE_ROUND(v0, v1, v2, v3)                                           \
    HALF_ROUND(v0, v1, v2, v3, 13, 16);                                        \
    HALF_ROUND(v2, v1, v0, v3, 17, 21);                                        \
    HALF_ROUND(v0, v1, v2, v3, 13, 16);                                        \
    HALF_ROUND(v2, v1, v0, v3, 17, 21);

/* k0 = 0x0123456789abcdef, k1 = 0xf1e2d3c4b5a69788, little-endian. */
static const uint8_t secret_key[16] __attribute__((aligned(8))) = {
    0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
    0x88, 0x97, 0xa6, 0xb5, 0xc4, 0xd3, 0xe2, 0xf1
};

__attribute__((noinline, noclone, noipa))
static uint64_t
siphash24(const void *src, unsigned long src_sz, const uint8_t key[16])
{
    const uint64_t *_key = (const uint64_t *)key;
    uint64_t k0 = _key[0];
    uint64_t k1 = _key[1];
    uint64_t b = (uint64_t)src_sz << 56;
    const uint64_t *in = (const uint64_t *)src;

    uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
    uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
    uint64_t v3 = k1 ^ 0x7465646279746573ULL;

    while (src_sz >= 8) {
        uint64_t mi = *in;
        in += 1;
        src_sz -= 8;
        v3 ^= mi;
        DOUBLE_ROUND(v0, v1, v2, v3);
        v0 ^= mi;
    }

    uint64_t t = 0;
    uint8_t *pt = (uint8_t *)&t;
    uint8_t *m = (uint8_t *)in;
    switch (src_sz) {
    case 7: pt[6] = m[6]; /* fallthrough */
    case 6: pt[5] = m[5]; /* fallthrough */
    case 5: pt[4] = m[4]; /* fallthrough */
    case 4: *((uint32_t *)&pt[0]) = *((uint32_t *)&m[0]); break;
    case 3: pt[2] = m[2]; /* fallthrough */
    case 2: pt[1] = m[1]; /* fallthrough */
    case 1: pt[0] = m[0];
    }
    b |= t;

    v3 ^= b;
    DOUBLE_ROUND(v0, v1, v2, v3);
    v0 ^= b;
    v2 ^= 0xff;
    DOUBLE_ROUND(v0, v1, v2, v3);
    DOUBLE_ROUND(v0, v1, v2, v3);
    return (v0 ^ v1) ^ (v2 ^ v3);
}

/* The helper the attacker is allowed to call: it returns only the tag.  The
 * key is never exposed to the caller. */
__attribute__((noinline, noclone, noipa))
uint64_t
victim_tag(void)
{
    return siphash24((const void *)0, 0, secret_key);
}

/* ------------------------------------------------------------------ */
/* Post-hoc oracle (experimenter side; runs after the measurement)     */
/* ------------------------------------------------------------------ */
int
oracle_report_v1(uint64_t recovered, uint64_t known_mask, uint64_t sink)
{
    int match = (known_mask == 0xff) && (recovered == secret_value_storage);
    printf("poc_v1 secret=%016llx recovered=%016llx known=%02x sink=%llu %s\n",
           (unsigned long long)secret_value_storage,
           (unsigned long long)recovered, (unsigned)(known_mask & 0xff),
           (unsigned long long)sink, match ? "MATCH" : "NO-MATCH");
    return match ? 0 : 1;
}

int
oracle_report_v2(uint64_t recovered, uint64_t known_mask, int arch_ok,
                 uint64_t sink)
{
    int match = arch_ok && (known_mask == 0xff) &&
                (recovered == secret_value_storage);
    printf("poc_v2 secret=%016llx recovered=%016llx known=%02x sink=%llu "
           "arch=%s %s\n",
           (unsigned long long)secret_value_storage,
           (unsigned long long)recovered, (unsigned)(known_mask & 0xff),
           (unsigned long long)sink, arch_ok ? "OK" : "BAD",
           match ? "MATCH" : "NO-MATCH");
    return match ? 0 : 1;
}

int
oracle_report_key(uint64_t rk0, uint64_t rk1, int bytes, int votes,
                  int total_votes, int arch_ok, uint64_t sink)
{
    uint64_t k0, k1;
    memcpy(&k0, secret_key, 8);
    memcpy(&k1, secret_key + 8, 8);
    int match = arch_ok && (bytes == 16) && (rk0 == k0) && (rk1 == k1);
    printf("siphash_attack k0=%016llx k1=%016llx "
           "rk0=%016llx rk1=%016llx "
           "bytes=%d/16 votes=%d/%d arch=%s sink=%llu %s\n",
           (unsigned long long)k0, (unsigned long long)k1,
           (unsigned long long)rk0, (unsigned long long)rk1,
           bytes, votes, total_votes, arch_ok ? "OK" : "BAD",
           (unsigned long long)sink, match ? "MATCH" : "NO-MATCH");
    return match ? 0 : 1;
}
