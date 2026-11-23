/*
 * Layer 3 verification: deferred prediction (`Wake` / `Avail`, Section III-C).
 *
 * Three independent phases, each with its own static load PCs:
 *
 *   A) Active deferred wakeup.
 *      Base and dependent both stream over large arrays, so both miss in L1D.
 *      The dependent dispatches while the base is still unresolved -> binds
 *      `Wake`; when the base completes, the dependent is woken with
 *      `actual(base) + diff`.
 *      Expect: egdiffDeferredBindings > 0, egdiffDeferredPredictions > 0.
 *
 *   B) Early-completion guard.
 *      The base streams (always misses) but the dependent is a hot scalar
 *      array, so the dependent completes *before* the base.  The binding must
 *      be discarded, not applied.
 *      Expect: egdiffDeferredIgnored > 0.
 *
 *   C) Normal (dispatch-time) prediction.
 *      The base completed earlier in the same iteration (21 intervening loads
 *      give distance d = 22), so the dependent reads a ready base value at
 *      dispatch instead of binding a wakeup.
 *      Expect: egdiffPredictions > 0.
 *
 * Every phase keeps the +40 relation consistent, so the architectural result
 * must be exact and numPredictionIncorrect must stay 0.
 */

#include <stdint.h>
#include <stdio.h>

#define ITER   320
#define SELEMS 512         /* 4 KiB per iteration => defeats the prefetcher */

#define NOISE 21           /* noise loads between base and dependent (d = 22) */

/* phase A / B: streaming arrays (deliberately larger than L1/L2) */
static volatile uint64_t A_big[ITER * SELEMS];
static volatile uint64_t B_big[ITER * SELEMS];

/* phase B / A intermediate (hot) */
static volatile uint64_t C_s[ITER];

/* phase B dependent (hot) */
static volatile uint64_t B_hot[ITER];

/* phase C */
static volatile uint64_t A_c[ITER];
static volatile uint64_t B_c[ITER];

/* constant scalars used as the 21 intervening loads in phase C (hot lines) */
static volatile uint64_t n0, n1, n2, n3, n4, n5, n6, n7, n8, n9, n10,
                        n11, n12, n13, n14, n15, n16, n17, n18, n19, n20;

static uint64_t
mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

int main(void)
{
    uint64_t expect = 0;

    for (int i = 0; i < ITER; i++) {
        uint64_t a = mix((uint64_t)i + 0x1000);
        uint64_t c = mix((uint64_t)i + 0x900000);

        A_big[(size_t)i * SELEMS] = a;
        B_big[(size_t)i * SELEMS] = a + 40;
        C_s[i] = c;
        B_hot[i] = a + 40;

        uint64_t ac = mix((uint64_t)i + 0x330000);
        A_c[i] = ac;
        B_c[i] = ac + 40;

        /* phases A and B each traverse the same (a, c, a+40) shape */
        expect += 2 * (a + c + (a + 40));
        expect += ac + (ac + 40);            /* phase C base + dependent     */
        expect += n0 + n1 + n2 + n3 + n4 + n5 + n6 + n7 + n8 + n9 + n10
                + n11 + n12 + n13 + n14 + n15 + n16 + n17 + n18 + n19 + n20;
    }

    volatile uint64_t sink = 0;

    /* ---- Phase A: both loads miss -> deferred wakeup ---- */
    for (int i = 0; i < ITER; i++) {
        uint64_t b = A_big[(size_t)i * SELEMS];   /* L_base  (miss) */
        uint64_t m = C_s[i];                      /* intermed (hot) */
        uint64_t t = B_big[(size_t)i * SELEMS];   /* L_dep   (miss) */
        sink += b + m + t;
    }

    /* ---- Phase B: base misses, dependent hits -> wakeup ignored ---- */
    for (int i = 0; i < ITER; i++) {
        uint64_t b = A_big[(size_t)i * SELEMS];   /* L_base  (miss) */
        uint64_t m = C_s[i];                      /* intermed (hot) */
        uint64_t t = B_hot[i];                    /* L_dep   (hot)  */
        sink += b + m + t;
    }

    /* ---- Phase C: base already completed -> normal dispatch prediction ---- */
    for (int i = 0; i < ITER; i++) {
        uint64_t b = A_c[i];                      /* L_base */
        uint64_t acc = 0;
        acc += n0;  acc += n1;  acc += n2;  acc += n3;  acc += n4;
        acc += n5;  acc += n6;  acc += n7;  acc += n8;  acc += n9;
        acc += n10; acc += n11; acc += n12; acc += n13; acc += n14;
        acc += n15; acc += n16; acc += n17; acc += n18; acc += n19;
        acc += n20;
        uint64_t t = B_c[i];                      /* L_dep (d = 22) */
        sink += b + acc + t;
    }

    printf("layer3 sink=%llu expect=%llu %s\n",
           (unsigned long long)sink, (unsigned long long)expect,
           (sink == expect) ? "OK" : "MISMATCH");
    return sink == expect ? 0 : 1;
}
