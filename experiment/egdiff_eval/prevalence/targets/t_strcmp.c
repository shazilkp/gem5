/*
 * t_strcmp.c -- control target: early-exit NUL-terminated strcmp.
 *
 * No external source.  The tagged buffer is the *secret* string (b); the
 * attacker presents the first string (a).  Comparison stops at the first
 * differing byte or at the terminating NUL, so the dynamic load count depends
 * on both the attacker input and the secret.
 *
 * Modes: "msg" varies the presented string, "key" varies the secret string,
 * "len" varies the shared prefix length (both strings identical that long).
 */
#include "harness.h"

#define STRMAX 128

static uint8_t qry[STRMAX + 1] __attribute__((aligned(64)));

__attribute__((noinline, used))
int eg_strcmp_early(const char *a, const char *b)
{
    for (;;) {
        unsigned x = (unsigned char)*a++;
        unsigned y = (unsigned char)*b++;
        if (x != y)
            return (int)x - (int)y;
        if (x == 0)
            return 0;
    }
}

static void make_string(uint8_t *dst, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)eg_rand64();
        if (c == 0)
            c = 'a';
        dst[i] = c;
    }
    dst[len] = 0;
}

int main(int argc, char **argv)
{
    eg_secure_buf sec = eg_secure_alloc("strcmp-secret");
    int mode = eg_parse_mode(argc, argv);
    int n = eg_ncalls(argc, argv);

    eg_seed(0x535452434D50ULL);
    make_string(sec.data, 64);
    make_string(qry, 64);

    if (argc > 1 && strcmp(argv[1], "probe") == 0) {
        eg_probe_install();
        for (int i = 0; i < 24; i++) {
            size_t k = (size_t)(eg_rand64() % 64);
            if (i % 3 == 0) {
                memcpy(qry, sec.data, 65);             /* full match  */
            } else if (i % 3 == 1) {
                memcpy(qry, sec.data, k);
                qry[k] = (uint8_t)('z' ^ 1);
                qry[k + 1] = 0;
            } else {
                make_string(sec.data, 32 + (i % 32));
                memcpy(qry, sec.data, k);
                qry[k] = 'q';
                qry[k + 1] = 0;
            }
            EG_PROBE_CALL(&sec, eg_strcmp_early((const char *)qry, (const char *)sec.data));
        }
        eg_probe_report("t_strcmp");
        printf("target=t_strcmp mode=probe calls=24 sink=%llu sec=%p\n",
               (unsigned long long)eg_sink, (void *)sec.data);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        if (mode == EG_MODE_MSG) {
            size_t k = (size_t)(eg_rand64() % 64);
            if ((i % 8) == 0) {
                memcpy(qry, sec.data, 65);             /* full match  */
            } else {
                memcpy(qry, sec.data, k);              /* prefix match */
                qry[k] = (uint8_t)(('a' + (eg_rand64() % 26)) | 0x80);
                qry[k + 1] = 0;
            }
        } else if (mode == EG_MODE_KEY) {
            make_string(sec.data, 16 + (size_t)(eg_rand64() % 48));
        } else {
            size_t len = 1 + (size_t)(eg_rand64() % 96);
            make_string(sec.data, len);
            memcpy(qry, sec.data, len + 1);
        }
        EG_RUN_CALL(eg_strcmp_early((const char *)qry, (const char *)sec.data));
    }

    printf("target=t_strcmp mode=%s calls=%d sink=%llu sec=%p\n",
           eg_mode_name(mode), n, (unsigned long long)eg_sink, (void *)sec.data);
    return 0;
}
