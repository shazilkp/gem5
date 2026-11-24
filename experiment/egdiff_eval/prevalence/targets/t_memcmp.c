/*
 * t_memcmp.c -- control target: early-exit byte-wise memcmp.
 *
 * No external source.  The tagged buffer is the *secret* operand (b), the
 * attacker presents the first operand (a).  The loop exits at the first
 * differing byte, so the number of dynamic loads is strongly data dependent
 * while the tail between the last secret byte load and the caller's next load
 * should stay constant.
 *
 * Modes: "msg" varies the attacker-presented bytes against a fixed secret,
 * "key" varies the secret against a fixed presented value, "len" varies n
 * with a and b fully equal (worst case: no early exit).
 */
#include "harness.h"

#define BUFSZ 256

static uint8_t qry[BUFSZ] __attribute__((aligned(64)));

__attribute__((noinline, used))
int eg_memcmp_early(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned x = a[i];
        unsigned y = b[i];
        if (x != y)
            return (int)x - (int)y;
    }
    return 0;
}

int main(int argc, char **argv)
{
    eg_secure_buf sec = eg_secure_alloc("memcmp-secret");
    int mode = eg_parse_mode(argc, argv);
    int n = eg_ncalls(argc, argv);

    eg_seed(0x4D454D434D50ULL);
    eg_fill(sec.data, BUFSZ);
    eg_fill(qry, BUFSZ);

    if (argc > 1 && strcmp(argv[1], "probe") == 0) {
        eg_probe_install();
        for (int i = 0; i < 24; i++) {
            size_t k = (size_t)(eg_rand64() % BUFSZ);
            eg_fill(qry, BUFSZ);
            if (i % 3 == 0) {
                memcpy(qry, sec.data, BUFSZ);          /* full match  */
            } else if (i % 3 == 1) {
                memcpy(qry, sec.data, k);              /* prefix match */
                if (k < BUFSZ)
                    qry[k] ^= 0x5A;
            } else {
                eg_fill(sec.data, BUFSZ);              /* vary secret  */
                memcpy(qry, sec.data, k);
            }
            EG_PROBE_CALL(&sec, eg_memcmp_early(qry, sec.data, BUFSZ));
        }
        eg_probe_report("t_memcmp");
        printf("target=t_memcmp mode=probe calls=24 sink=%llu sec=%p\n",
               (unsigned long long)eg_sink, (void *)sec.data);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        size_t len = BUFSZ;
        if (mode == EG_MODE_MSG) {
            size_t k = (size_t)(eg_rand64() % BUFSZ);
            eg_fill(qry, BUFSZ);
            if ((i % 8) == 0) {
                memcpy(qry, sec.data, BUFSZ);          /* full match  */
            } else {
                memcpy(qry, sec.data, k);              /* prefix match */
                if (k < BUFSZ)
                    qry[k] ^= (uint8_t)(1 + (eg_rand64() & 0x7F));
            }
        } else if (mode == EG_MODE_KEY) {
            eg_fill(sec.data, BUFSZ);
        } else {
            len = 1 + (size_t)(eg_rand64() % BUFSZ);
            memcpy(qry, sec.data, len);                /* no early exit */
        }
        EG_RUN_CALL(eg_memcmp_early(qry, sec.data, len));
    }

    printf("target=t_memcmp mode=%s calls=%d sink=%llu sec=%p\n",
           eg_mode_name(mode), n, (unsigned long long)eg_sink, (void *)sec.data);
    return 0;
}
