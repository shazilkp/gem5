/*
 * t_hashlookup.c -- control target: open-addressing hash table lookup.
 *
 * No external source.  The tagged buffer holds the 64-slot table (the secret);
 * the attacker supplies the 32-byte query from an ordinary array.  A lookup
 * probes linearly until it finds a match or an empty slot, so the number of
 * dynamic loads depends on the query and on the table contents.
 *
 * Modes: "msg" varies the query against a fixed table, "key" varies the table
 * contents against a fixed query.  There is no length parameter.
 */
#include "harness.h"

#define HSLOTS 64
#define KEYLEN 32

typedef struct {
    uint8_t used;
    uint8_t pad[7];
    uint8_t key[KEYLEN];
} hslot;                                  /* 40 bytes; 64 * 40 = 2560 B */

static uint8_t qry[KEYLEN] __attribute__((aligned(64)));
static uint8_t kbuf[HSLOTS][KEYLEN];

static uint64_t lrand(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static uint64_t fnv1a(const uint8_t *k, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= k[i];
        h *= 1099511628211ULL;
    }
    return h;
}

__attribute__((noinline, used))
int eg_hash_lookup(const hslot *tab, uint32_t mask, const uint8_t *q)
{
    uint32_t h = (uint32_t)fnv1a(q, KEYLEN) & mask;
    for (uint32_t probe = 0; probe <= mask; probe++) {
        const hslot *s = &tab[h];
        if (!s->used)
            return 0;
        int eq = 1;
        for (int i = 0; i < KEYLEN; i++) {
            if (s->key[i] != q[i]) {
                eq = 0;
                break;
            }
        }
        if (eq)
            return 1;
        h = (h + 1) & mask;
    }
    return -1;
}

static void hs_insert(hslot *tab, uint32_t mask, const uint8_t *k)
{
    uint32_t h = (uint32_t)fnv1a(k, KEYLEN) & mask;
    for (uint32_t probe = 0; probe <= mask; probe++) {
        if (!tab[h].used) {
            tab[h].used = 1;
            memcpy(tab[h].key, k, KEYLEN);
            return;
        }
        h = (h + 1) & mask;
    }
}

/* Rebuild the table from scratch with `count` random keys. */
static void hs_build(hslot *tab, uint32_t mask, uint64_t seed, int count,
                     const uint8_t *must_insert)
{
    uint64_t s = seed;
    memset(tab, 0, HSLOTS * sizeof(hslot));
    for (int i = 0; i < count; i++) {
        uint8_t k[KEYLEN];
        for (int j = 0; j < KEYLEN; j++)
            k[j] = (uint8_t)lrand(&s);
        memcpy(kbuf[i], k, KEYLEN);
        hs_insert(tab, mask, k);
    }
    if (must_insert)
        hs_insert(tab, mask, must_insert);
}

int main(int argc, char **argv)
{
    eg_secure_buf sec = eg_secure_alloc("hash-table");
    hslot *tab = (hslot *)sec.data;
    uint32_t mask = HSLOTS - 1;
    int mode = eg_parse_mode(argc, argv);
    int n = eg_ncalls(argc, argv);

    hs_build(tab, mask, 0x484153485441424CULL, 48, NULL);
    memcpy(qry, kbuf[0], KEYLEN);

    if (argc > 1 && strcmp(argv[1], "probe") == 0) {
        eg_probe_install();
        for (int i = 0; i < 24; i++) {
            if (i % 3 == 2) {
                hs_build(tab, mask, 0x100 + (uint64_t)i, 48, NULL);
                for (int j = 0; j < KEYLEN; j++)
                    qry[j] = (uint8_t)(0xA0 + i + j);
            } else {
                hs_build(tab, mask, 0x200 + (uint64_t)i, 48, NULL);
                memcpy(qry, kbuf[(size_t)(i * 7) % 48], KEYLEN);
                if (i % 2)
                    qry[0] ^= 0xFF;                /* force a miss */
            }
            EG_PROBE_CALL(&sec, eg_hash_lookup(tab, mask, qry));
        }
        eg_probe_report("t_hashlookup");
        printf("target=t_hashlookup mode=probe calls=24 sink=%llu tab=%p\n",
               (unsigned long long)eg_sink, (void *)tab);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        if (mode == EG_MODE_MSG) {
            /* table fixed, query varies: hit on an existing key or a miss */
            if ((i % 4) == 3) {
                for (int j = 0; j < KEYLEN; j++)
                    qry[j] = (uint8_t)eg_rand64();
            } else {
                memcpy(qry, kbuf[(size_t)(eg_rand64() % 48)], KEYLEN);
            }
        } else {
            /* query fixed, table content varies */
            uint8_t must[KEYLEN];
            int want_hit = (i & 1);
            for (int j = 0; j < KEYLEN; j++)
                must[j] = qry[j];
            hs_build(tab, mask, 0x9000 + (uint64_t)i, 40,
                     want_hit ? must : NULL);
        }
        EG_RUN_CALL(eg_hash_lookup(tab, mask, qry));
    }

    printf("target=t_hashlookup mode=%s calls=%d sink=%llu tab=%p\n",
           eg_mode_name(mode), n, (unsigned long long)eg_sink, (void *)tab);
    return 0;
}
