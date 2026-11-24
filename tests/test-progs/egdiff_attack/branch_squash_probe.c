/*
 * branch_squash_probe.c -- is a speculative cache fill tied to the
 * value-misprediction squash path, or is it a general cache property?
 *
 * The program trains a conditional branch to be taken, then flips the
 * condition so the branch mispredicts.  The wrong path touches one probe line
 * with a known index; the branch condition is computed through a long
 * register-only division chain, so the probe's cache access has time to
 * complete before the branch resolves and the wrong path is squashed.  After
 * the squash, the probe line is timed against a cold line.
 *
 * Modes:
 *   arch    -- condition stays true, so the bad path executes architecturally
 *              (positive control: the line must be hot)
 *   squash  -- condition flips, so the bad path is squashed
 *   (both modes flush the probe array before the measured access)
 *
 * If `squash` leaves the probe line hot, speculative fill survival is a
 * general cache property and is not specific to IEW::squashDueToMemOrder.
 *
 * Build:
 *   riscv64-linux-gnu-gcc -static -O2 -fno-pic -no-pie \
 *       -o branch_squash_probe branch_squash_probe.c
 * Run:
 *   EGDIFF_ON=0 build/RISCV/gem5.opt --cpu-type=O3CPU --caches \
 *       configs/example/egdiff.py --cmd=.../branch_squash_probe --options=squash
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LINES      256
#define LINE       64
#define PROBE_SZ   (LINES * LINE)
#define FLUSH_SZ   (128 * 1024)
#define HOT_LINE   0x42
#define COLD_LINE  0x13
#define TRAIN      96

static uint8_t probe[PROBE_SZ] __attribute__((aligned(LINE)));
static volatile uint8_t flushbuf[FLUSH_SZ];
static volatile uint64_t sink;
static volatile uint64_t seed;
static volatile uint64_t take;
static volatile int hot_idx;

static inline uint64_t
rdcycle(void)
{
    uint64_t c;
    asm volatile("csrrs %0, cycle, x0" : "=r"(c) :: "memory");
    return c;
}

static inline uint64_t
time_line(int k)
{
    volatile uint8_t *q = probe + (size_t)k * LINE;
    uint64_t t1 = rdcycle();
    uint8_t v = *q;
    asm volatile("" :: "r"(v) : "memory");
    return rdcycle() - t1;
}

static void
flush_l1(void)
{
    for (int i = 0; i < FLUSH_SZ; i += LINE)
        (void)flushbuf[i];
}

/* Register-only dependency chain: keeps the branch condition unresolved for a
 * few hundred cycles while the wrong path is fetched and executed. */
__attribute__((noinline, noclone))
static uint64_t
slow_cond(uint64_t s)
{
    uint64_t x = s | 1;
    x /= (s + 2);
    x /= (s + 3);
    x /= (s + 4);
    x /= (s + 5);
    x /= (s + 6);
    x /= (s + 7);
    x /= (s + 8);
    x /= (s + 9);
    x /= (s + 10);
    x /= (s + 11);
    x /= (s + 12);
    x /= (s + 13);
    return x;
}

int
main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "squash";
    int squash = (strcmp(mode, "squash") == 0);
    int none = (strcmp(mode, "none") == 0);
    memset(probe, 0, sizeof(probe));
    seed = 0x51ED;
    hot_idx = 0;                       /* training touches line 0, not HOT_LINE */

    /* Train the branch.  `none` trains it not-taken so the bad path is never
     * fetched; `arch` and `squash` train it taken.  The condition is
     * deliberately not short-circuitable: it depends on slow_cond() and `take`. */
    for (int i = 0; i < TRAIN; i++) {
        take = none ? 0 : 1;
        if (((slow_cond(seed) ^ take) & 1) == 1) {
            volatile uint8_t *p = probe + (size_t)hot_idx * LINE;
            sink += *p;
        } else {
            sink += 0x55;
        }
    }

    /* Make the measured line cold. */
    flush_l1();
    asm volatile("fence.i" ::: "memory");

    /* Attack / architectural control.  `take = 0` flips the condition, so a
     * branch trained taken will mispredict; `take = 1` executes the bad path
     * architecturally. */
    take = (squash || none) ? 0 : 1;
    /* In `none`, even a speculative wrong-path fetch must not touch the
     * measured line; route it to a different line. */
    hot_idx = none ? 0x7E : HOT_LINE;
    if (((slow_cond(seed) ^ take) & 1) == 1) {
        volatile uint8_t *p = probe + (size_t)hot_idx * LINE;
        sink += *p;
    } else {
        sink += 0xAA;
    }

    /* Measure after the branch has resolved and any squash has happened.
     * The first access after the flush is a known artifact (it is fast even
     * when no bad path executed), so absorb it on a dummy line, then measure
     * the candidate line twice. */
    uint64_t dummy = time_line(0x7F);
    uint64_t hot = time_line(HOT_LINE);
    uint64_t cold = time_line(COLD_LINE);
    uint64_t line0 = time_line(0);

    printf("mode=%s dummy=%llu hot=%llu cold=%llu line0=%llu sink=%llu\n",
           mode, (unsigned long long)dummy, (unsigned long long)hot,
           (unsigned long long)cold, (unsigned long long)line0,
           (unsigned long long)sink);
    return 0;
}
