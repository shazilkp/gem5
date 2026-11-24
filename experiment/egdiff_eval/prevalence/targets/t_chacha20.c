/*
 * t_chacha20.c -- prevalence target: Monocypher crypto_chacha20_djb().
 *
 * The tagged buffer is the 32-byte key.  crypto_chacha20_djb loads the key
 * exactly once, at function entry (load32_le_buf(input + 4, key, 8)); the
 * per-block loop only re-reads the expanded state from the stack.  So the
 * distance from the key-buffer read to the caller's next load is expected to
 * grow with text_size.
 *
 * Modes: "msg" varies the plaintext, "key" varies the key, "len" varies the
 * length (1..256, fixed key and plaintext).
 */
#include "harness.h"
#include "../third_party/Monocypher/src/monocypher.h"

#define KEYLEN  32
#define PTMAX   512

int main(int argc, char **argv)
{
    eg_secure_buf sk = eg_secure_alloc("chacha20-key");
    uint8_t ct[PTMAX + 64];
    uint8_t pt[PTMAX];
    uint8_t nonce[8];
    int mode = eg_parse_mode(argc, argv);
    int n = eg_ncalls(argc, argv);

    eg_seed(0x434841434841ULL);
    eg_fill(sk.data, KEYLEN);
    eg_fill(pt, PTMAX);
    eg_fill(nonce, sizeof nonce);

    if (argc > 1 && strcmp(argv[1], "probe") == 0) {
        eg_probe_install();
        for (int i = 0; i < 24; i++) {
            size_t len = 1 + (size_t)(eg_rand64() % 256);
            if (i % 4 == 0)
                len = 64;
            if (i % 4 == 1)
                len = 256;
            if (i % 4 == 2)
                len = 1;
            if (i % 5 == 0)
                eg_fill(sk.data, KEYLEN);
            EG_PROBE_CALL(&sk, crypto_chacha20_djb(ct, pt, len, sk.data, nonce, 0));
        }
        eg_probe_report("t_chacha20");
        printf("target=t_chacha20 mode=probe calls=24 sink=%llu key=%p\n",
               (unsigned long long)eg_sink, (void *)sk.data);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        size_t len = 256;
        if (mode == EG_MODE_MSG) {
            eg_fill(pt, PTMAX);
        } else if (mode == EG_MODE_KEY) {
            eg_fill(sk.data, KEYLEN);
        } else {
            len = 1 + (size_t)(eg_rand64() % 256);
        }
        EG_RUN_CALL(crypto_chacha20_djb(ct, pt, len, sk.data, nonce, 0));
    }

    printf("target=t_chacha20 mode=%s calls=%d sink=%llu key=%p\n",
           eg_mode_name(mode), n, (unsigned long long)eg_sink, (void *)sk.data);
    return 0;
}
