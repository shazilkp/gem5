/*
 * t_aead.c -- prevalence target: Monocypher crypto_aead_lock().
 *
 * Composite target: XChaCha20-Poly1305.  The tagged buffer is the 32-byte key.
 * crypto_aead_lock() -> crypto_aead_init_x() -> crypto_chacha20_h(), which is
 * the only place the raw key buffer is read; afterwards the derived key lives
 * in the on-stack aead context and is re-read from the stack.
 *
 * Modes: "msg" varies the plaintext, "key" varies the key, "len" varies the
 * plaintext length (1..256).  Associated data is a fixed 16-byte block.
 */
#include "harness.h"
#include "../third_party/Monocypher/src/monocypher.h"

#define KEYLEN  32
#define PTMAX   512
#define ADLEN   16

int main(int argc, char **argv)
{
    eg_secure_buf sk = eg_secure_alloc("aead-key");
    uint8_t ct[PTMAX + 64];
    uint8_t mac[16];
    uint8_t pt[PTMAX];
    uint8_t ad[ADLEN];
    uint8_t nonce[24];
    int mode = eg_parse_mode(argc, argv);
    int n = eg_ncalls(argc, argv);

    eg_seed(0x414541444C4F434BULL);
    eg_fill(sk.data, KEYLEN);
    eg_fill(pt, PTMAX);
    eg_fill(ad, ADLEN);
    eg_fill(nonce, sizeof nonce);

    if (argc > 1 && strcmp(argv[1], "probe") == 0) {
        eg_probe_install();
        for (int i = 0; i < 24; i++) {
            size_t len = 1 + (size_t)(eg_rand64() % 256);
            if (i % 4 == 0)
                len = 1;
            if (i % 4 == 1)
                len = 64;
            if (i % 4 == 2)
                len = 256;
            if (i % 5 == 0)
                eg_fill(sk.data, KEYLEN);
            EG_PROBE_CALL(&sk, (crypto_aead_lock(ct, mac, sk.data, nonce, ad,
                                                 ADLEN, pt, len), 0));
        }
        eg_probe_report("t_aead");
        printf("target=t_aead mode=probe calls=24 sink=%llu key=%p\n",
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
        EG_RUN_CALL((crypto_aead_lock(ct, mac, sk.data, nonce, ad, ADLEN,
                                      pt, len), 0));
    }

    printf("target=t_aead mode=%s calls=%d sink=%llu key=%p\n",
           eg_mode_name(mode), n, (unsigned long long)eg_sink, (void *)sk.data);
    return 0;
}
