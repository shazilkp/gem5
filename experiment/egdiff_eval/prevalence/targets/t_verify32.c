/*
 * t_verify32.c -- prevalence target: Monocypher crypto_verify32().
 *
 * crypto_verify32 is constant time: it reads all 32 bytes of both operands
 * once, with no data-dependent branches and no data-dependent addresses.
 * The tagged buffer is the *second* operand (b[]), i.e. the secret.  It is
 * passed second on purpose: the shared load64_le() helper then reads the
 * secret in the final calls, so the last execution of a key-reading PC is a
 * real key read rather than a read of the attacker operand.
 *
 * Modes: "msg" varies the attacker-presented first operand with the secret
 * fixed; "key" varies the secret with the presented value fixed.  There is no
 * length parameter, so there is no "len" mode.
 */
#include "harness.h"
#include "../third_party/Monocypher/src/monocypher.h"

#define KEYLEN 32

int main(int argc, char **argv)
{
    eg_secure_buf sk = eg_secure_alloc("verify32-secret");
    uint8_t other[64];
    int mode = eg_parse_mode(argc, argv);
    int n = eg_ncalls(argc, argv);

    eg_seed(0x563332563332ULL);
    eg_fill(sk.data, KEYLEN);
    eg_fill(other, sizeof other);

    if (argc > 1 && strcmp(argv[1], "probe") == 0) {
        eg_probe_install();
        for (int i = 0; i < 24; i++) {
            eg_fill(other, KEYLEN);
            if (i % 3 == 0)
                memcpy(other, sk.data, KEYLEN);        /* equal */
            else if (i % 3 == 1)
                memcpy(other, sk.data, KEYLEN - 3);    /* near equal */
            EG_PROBE_CALL(&sk, crypto_verify32(other, sk.data));
        }
        eg_probe_report("t_verify32");
        printf("target=t_verify32 mode=probe calls=24 sink=%llu key=%p\n",
               (unsigned long long)eg_sink, (void *)sk.data);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        if (mode == EG_MODE_MSG) {
            eg_fill(other, KEYLEN);
            if (i % 4 == 0)
                memcpy(other, sk.data, KEYLEN);
        } else if (mode == EG_MODE_KEY) {
            eg_fill(sk.data, KEYLEN);
        }
        EG_RUN_CALL(crypto_verify32(other, sk.data));
    }

    printf("target=t_verify32 mode=%s calls=%d sink=%llu key=%p\n",
           eg_mode_name(mode), n, (unsigned long long)eg_sink, (void *)sk.data);
    return 0;
}
