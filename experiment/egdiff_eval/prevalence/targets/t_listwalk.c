/*
 * t_listwalk.c -- control target: singly linked list key search.
 *
 * No external source.  The tagged buffer holds the list nodes (the secret);
 * the attacker supplies the 32-byte query from an ordinary array.  The walk
 * stops at the matching node or at the end of the list, so the number of
 * dynamic loads depends on the query and on where the match sits.
 *
 * Modes: "msg" varies the query against a fixed list, "key" varies the list
 * contents against a fixed query.  There is no length parameter.
 */
#include "harness.h"

#define NNODES 24
#define KEYLEN 32

typedef struct egnode {
    struct egnode *next;
    uint8_t key[KEYLEN];
} egnode;                                 /* 40 bytes; 24 * 40 = 960 B */

static uint8_t qry[KEYLEN] __attribute__((aligned(64)));
static uint8_t nkeys[NNODES][KEYLEN];

static uint64_t lrand(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

__attribute__((noinline, used))
int eg_list_walk(const egnode *head, const uint8_t *q)
{
    for (const egnode *p = head; p != NULL; p = p->next) {
        int eq = 1;
        for (int i = 0; i < KEYLEN; i++) {
            if (p->key[i] != q[i]) {
                eq = 0;
                break;
            }
        }
        if (eq)
            return 1;
    }
    return 0;
}

static egnode *ll_build(egnode *nodes, uint64_t seed, const uint8_t *must)
{
    uint64_t s = seed;
    for (int i = 0; i < NNODES; i++) {
        nodes[i].next = (i + 1 < NNODES) ? &nodes[i + 1] : NULL;
        for (int j = 0; j < KEYLEN; j++)
            nkeys[i][j] = (uint8_t)lrand(&s);
        memcpy(nodes[i].key, nkeys[i], KEYLEN);
    }
    if (must)
        memcpy(nodes[(size_t)(lrand(&s) % NNODES)].key, must, KEYLEN);
    return &nodes[0];
}

int main(int argc, char **argv)
{
    eg_secure_buf sec = eg_secure_alloc("list-nodes");
    egnode *nodes = (egnode *)sec.data;
    egnode *head;
    int mode = eg_parse_mode(argc, argv);
    int n = eg_ncalls(argc, argv);

    head = ll_build(nodes, 0x4C49535457414C4BULL, NULL);
    memcpy(qry, nkeys[0], KEYLEN);

    if (argc > 1 && strcmp(argv[1], "probe") == 0) {
        eg_probe_install();
        for (int i = 0; i < 24; i++) {
            if (i % 3 == 2) {
                head = ll_build(nodes, 0x300 + (uint64_t)i, NULL);
                for (int j = 0; j < KEYLEN; j++)
                    qry[j] = (uint8_t)(0x5A + i + j);
            } else {
                head = ll_build(nodes, 0x400 + (uint64_t)i, NULL);
                memcpy(qry, nkeys[(size_t)(i * 5) % NNODES], KEYLEN);
                if (i % 2)
                    qry[0] ^= 0xFF;                 /* force a miss */
            }
            EG_PROBE_CALL(&sec, eg_list_walk(head, qry));
        }
        eg_probe_report("t_listwalk");
        printf("target=t_listwalk mode=probe calls=24 sink=%llu nodes=%p\n",
               (unsigned long long)eg_sink, (void *)nodes);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        if (mode == EG_MODE_MSG) {
            /* list fixed, query varies: hit a random node or miss */
            if ((i % 4) == 3) {
                for (int j = 0; j < KEYLEN; j++)
                    qry[j] = (uint8_t)eg_rand64();
            } else {
                memcpy(qry, nkeys[(size_t)(eg_rand64() % NNODES)], KEYLEN);
            }
        } else {
            /* query fixed, list content varies */
            uint8_t must[KEYLEN];
            int want_hit = (i & 1);
            memcpy(must, qry, KEYLEN);
            head = ll_build(nodes, 0xA000 + (uint64_t)i,
                            want_hit ? must : NULL);
        }
        EG_RUN_CALL(eg_list_walk(head, qry));
    }

    printf("target=t_listwalk mode=%s calls=%d sink=%llu nodes=%p\n",
           eg_mode_name(mode), n, (unsigned long long)eg_sink, (void *)nodes);
    return 0;
}
