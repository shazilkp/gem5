/*
 * t_blake2b.c -- prevalence target: Monocypher crypto_blake2b_keyed().
 *
 * The tagged buffer is the 32-byte key.  crypto_blake2b_keyed() calls
 * crypto_blake2b_keyed_init(), which hashes the key through the *same*
 * crypto_blake2b_update() entry point that later consumes the message, so
 * static argument provenance cannot separate key reads from message reads.
 * The dynamic PROT_NONE fault probe can, and is authoritative here; the key
 * buffer is only ever touched while the key is being absorbed.
 *
 * Modes: "msg" varies the message, "key" varies the key, "len" varies the
 * message length (0..255).
 *
 * Note: crypto_blake2b_keyed() returns void, so the call is wrapped in a comma
 * expression to give the measurement macro a value to consume.
 */
#include "harness.h"
#include "../third_party/Monocypher/src/monocypher.h"

#define KEYLEN  32
#define MSGMAX  512

int main(int argc, char **argv)
{
    eg_secure_buf sk = eg_secure_alloc("blake2b-key");
    uint8_t hash[64];
    uint8_t msg[MSGMAX];
    int mode = eg_parse_mode(argc, argv);
    int n = eg_ncalls(argc, argv);

    eg_seed(0x424C414B4532ULL);
    eg_fill(sk.data, KEYLEN);
    eg_fill(msg, MSGMAX);

    if (argc > 1 && strcmp(argv[1], "probe") == 0) {
        eg_probe_install();
        for (int i = 0; i < 24; i++) {
            size_t len = 1 + (size_t)(eg_rand64() % 255);
            if (i % 4 == 0)
                len = 0;
            if (i % 4 == 1)
                len = 64;
            if (i % 4 == 2)
                len = 128;
            if (i % 5 == 0)
                eg_fill(sk.data, KEYLEN);
            EG_PROBE_CALL(&sk, (crypto_blake2b_keyed(hash, 32, sk.data, KEYLEN,
                                                    msg, len), 0));
        }
        eg_probe_report("t_blake2b");
        printf("target=t_blake2b mode=probe calls=24 sink=%llu key=%p\n",
               (unsigned long long)eg_sink, (void *)sk.data);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        size_t len = 64;
        if (mode == EG_MODE_MSG) {
            eg_fill(msg, 128);
        } else if (mode == EG_MODE_KEY) {
            eg_fill(sk.data, KEYLEN);
        } else {
            len = (size_t)(eg_rand64() % 256);
        }
        EG_RUN_CALL((crypto_blake2b_keyed(hash, 32, sk.data, KEYLEN, msg, len), 0));
    }

    printf("target=t_blake2b mode=%s calls=%d sink=%llu key=%p\n",
           eg_mode_name(mode), n, (unsigned long long)eg_sink, (void *)sk.data);
    return 0;
}
