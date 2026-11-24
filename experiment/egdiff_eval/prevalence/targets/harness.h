/*
 * harness.h -- shared scaffolding for the EgDiff prevalence study.
 *
 * Single-translation-unit use only: the marker functions and marker words are
 * *defined* here on purpose, so every target binary contains exactly one copy
 * of each marker and therefore one stable static PC per marker.
 *
 * The two marker loads delimit a measured call:
 *
 *      eg_marker_call_begin()   ... target call ...   eg_marker_after_call()
 *
 * `eg_marker_call_begin` runs before the call; `eg_marker_after_call` runs
 * immediately after the call returns (T2 in the study definition).
 *
 * Sensitive buffers are two-page mmap'd regions: page 0 carries an ASCII tag,
 * page 1 holds the sensitive bytes and is page aligned.  A plain in-process
 * shadow copy of page 1 is kept so that the optional SIGSEGV fault probe can
 * emulate loads while the real page stays PROT_NONE (see below).
 */
#ifndef EG_PREVALENCE_HARNESS_H
#define EG_PREVALENCE_HARNESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>

#define EG_TAG_MAGIC "EGDIFFv1-KEYBUF"

#define EG_UNUSED __attribute__((unused))

/* ------------------------------------------------------------------------ */
/* Tagged, page-aligned sensitive buffers                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t *region;    /* mmap base; page 0 holds the ASCII tag           */
    uint8_t *data;      /* page-aligned sensitive page (page 1)            */
    uint8_t *shadow;    /* plain RW copy of `data`, used by the probe      */
    size_t   page;
    size_t   capacity;  /* == page */
} eg_secure_buf;

static EG_UNUSED eg_secure_buf eg_secure_alloc(const char *what)
{
    eg_secure_buf b;
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0)
        ps = 4096;
    b.page = (size_t)ps;
    b.capacity = b.page;
    b.region = (uint8_t *)mmap(NULL, 2 * b.page, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b.region == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }
    b.data = b.region + b.page;
    b.shadow = (uint8_t *)calloc(1, b.page);
    if (!b.shadow) {
        perror("calloc");
        exit(2);
    }
    memset(b.data, 0, b.page);
    snprintf((char *)b.region, 64, "%s:%s", EG_TAG_MAGIC, what);
    return b;
}

/* Copy the live sensitive page into the shadow (call after any write). */
static EG_UNUSED void eg_secure_sync(eg_secure_buf *b)
{
    memcpy(b->shadow, b->data, b->page);
}

/* ---- marker loads ------------------------------------------------------- */

volatile uint64_t eg_marker_word_begin = 0x1111111111111111ULL;
volatile uint64_t eg_marker_word_after = 0x2222222222222222ULL;
volatile uint64_t eg_sink;              /* keeps results observable */

__attribute__((noinline)) uint64_t eg_marker_call_begin(void)
{
    return eg_marker_word_begin;
}

__attribute__((noinline)) uint64_t eg_marker_after_call(void)
{
    uint64_t v = eg_marker_word_after;
    eg_sink ^= v;                       /* different body shape from the above */
    return v;
}

/* ---- input mode / call count ------------------------------------------- */

enum { EG_MODE_MSG = 0, EG_MODE_KEY = 1, EG_MODE_LEN = 2 };

static EG_UNUSED int eg_parse_mode(int argc, char **argv)
{
    if (argc < 2)
        return EG_MODE_MSG;
    if (strcmp(argv[1], "key") == 0)
        return EG_MODE_KEY;
    if (strcmp(argv[1], "len") == 0)
        return EG_MODE_LEN;
    return EG_MODE_MSG;
}

static EG_UNUSED const char *eg_mode_name(int mode)
{
    switch (mode) {
    case EG_MODE_KEY: return "key";
    case EG_MODE_LEN: return "len";
    default:          return "msg";
    }
}

static EG_UNUSED int eg_ncalls(int argc, char **argv)
{
    if (argc < 3)
        return 300;
    int n = atoi(argv[2]);
    return n > 0 ? n : 300;
}

/* ---- deterministic PRNG (xorshift64*) ---------------------------------- */

static uint64_t eg_rng_state = 0x9E3779B97F4A7C15ULL;

static EG_UNUSED void eg_seed(uint64_t s)
{
    eg_rng_state = s ? s : 0x9E3779B97F4A7C15ULL;
}

static EG_UNUSED uint64_t eg_rand64(void)
{
    uint64_t x = eg_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    eg_rng_state = x;
    return x;
}

static EG_UNUSED void eg_fill(uint8_t *p, size_t n)
{
    while (n--)
        *p++ = (uint8_t)eg_rand64();
}

/* Run one measured call: begin marker, call, T2 marker.
 *
 * The call's result is *consumed* (stored to the volatile sink) rather than
 * discarded: GCC's IPA purity analysis would otherwise delete a call to a
 * same-translation-unit pure function (e.g. the control loops) whose result is
 * unused, silently removing the very loads being measured.  The store adds no
 * load to the window. */
#define EG_RUN_CALL(EXPR)                                                   \
    do {                                                                    \
        eg_sink = eg_marker_call_begin();   /* store only: no extra load */ \
        eg_sink = (uint64_t)(EXPR);                                         \
        eg_sink ^= eg_marker_after_call();                                  \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Optional dynamic cross-check: PROT_NONE + SIGSEGV fault-and-emulate      */
/*                                                                          */
/* The sensitive page is made PROT_NONE for the whole call.  Each load that  */
/* touches it raises SIGSEGV; the handler decodes the faulting load, reads   */
/* the value out of the plain shadow copy, writes the destination register,  */
/* advances the PC past the load and returns.  The page therefore stays      */
/* protected, so *every* sensitive load in the call is reported, not just    */
/* the first one.  Returns the set of PCs that read the sensitive page.      */
/* ------------------------------------------------------------------------ */

#define EG_MAX_FAULT_PCS 4096

static uint8_t  *eg_probe_data;
static uint8_t  *eg_probe_shadow;
static size_t    eg_probe_page_sz;
static volatile int eg_probe_armed;
static volatile int eg_probe_bad;         /* unsupported fault / store */
static uintptr_t eg_fault_pcs[EG_MAX_FAULT_PCS];
static volatile int eg_nfault;    /* distinct-PC table fill, capped */
static volatile uint64_t eg_fault_total;  /* every emulated fault, never capped */
static struct sigaction eg_old_segv;

/* Decode one load instruction at `pc`, emulate it from the shadow, skip it. */
static int eg_emulate_load(ucontext_t *uc, uintptr_t pc, uintptr_t addr)
{
    const uint16_t *c16 = (const uint16_t *)pc;
    uint16_t lo = c16[0];
    unsigned long *g = uc->uc_mcontext.__gregs;
    int rd, rs1, len, size, sign;
    int64_t off;

    if ((lo & 3) != 3) {                       /* compressed (16-bit) */
        unsigned q = lo & 3, f3 = (lo >> 13) & 7;
        len = 2;
        if (q == 0 && f3 == 3) {               /* c.ld    */
            rd = 8 + ((lo >> 2) & 7); rs1 = 8 + ((lo >> 7) & 7);
            off = (((lo >> 10) & 7) << 3) | (((lo >> 5) & 3) << 6);
            size = 8; sign = 0;
        } else if (q == 0 && f3 == 2) {        /* c.lw    */
            rd = 8 + ((lo >> 2) & 7); rs1 = 8 + ((lo >> 7) & 7);
            off = (((lo >> 10) & 7) << 3) | (((lo >> 6) & 1) << 2) |
                  (((lo >> 5) & 1) << 6);
            size = 4; sign = 1;
        } else if (q == 2 && f3 == 3) {        /* c.ldsp  */
            rd = (lo >> 7) & 31; rs1 = 2;
            off = (((lo >> 12) & 1) << 5) | (((lo >> 5) & 3) << 3) |
                  (((lo >> 2) & 7) << 6);
            size = 8; sign = 0;
        } else if (q == 2 && f3 == 2) {        /* c.lwsp  */
            rd = (lo >> 7) & 31; rs1 = 2;
            off = (((lo >> 12) & 1) << 5) | (((lo >> 4) & 7) << 2) |
                  (((lo >> 2) & 3) << 6);
            size = 4; sign = 1;
        } else {
            return 0;                          /* unsupported compressed */
        }
    } else {                                   /* 32-bit */
        uint32_t w = *(const uint32_t *)pc;
        unsigned op = w & 0x7f, f3 = (w >> 12) & 7;
        rd = (w >> 7) & 31; rs1 = (w >> 15) & 31;
        off = (int64_t)(int32_t)w >> 20;
        len = 4;
        if (op != 0x03)                        /* LOAD opcode only */
            return 0;
        switch (f3) {
        case 0: size = 1; sign = 1; break;     /* lb  */
        case 1: size = 2; sign = 1; break;     /* lh  */
        case 2: size = 4; sign = 1; break;     /* lw  */
        case 3: size = 8; sign = 0; break;     /* ld  */
        case 4: size = 1; sign = 0; break;     /* lbu */
        case 5: size = 2; sign = 0; break;     /* lhu */
        case 6: size = 4; sign = 0; break;     /* lwu */
        default: return 0;
        }
    }

    /* Cross-check the decoded address against the kernel-reported one. */
    if ((uintptr_t)g[rs1] + (uintptr_t)(intptr_t)off != addr)
        return 0;

    const volatile uint8_t *p =
        (const volatile uint8_t *)(eg_probe_shadow + (addr - (uintptr_t)eg_probe_data));
    uint64_t raw = 0;
    for (int i = 0; i < size; i++)
        raw |= ((uint64_t)p[i]) << (8 * i);
    if (sign) {
        if (size == 1)      raw = (uint64_t)(int64_t)(int8_t)raw;
        else if (size == 2) raw = (uint64_t)(int64_t)(int16_t)raw;
        else if (size == 4) raw = (uint64_t)(int64_t)(int32_t)raw;
    }
    if (rd != 0)
        g[rd] = raw;
    g[0] = pc + (uintptr_t)len;                /* gregs[0] is the guest PC */
    return 1;
}

static void eg_segv_handler(int sig, siginfo_t *si, void *uctx)
{
    ucontext_t *uc = (ucontext_t *)uctx;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.__gregs[0];
    uintptr_t addr = (uintptr_t)si->si_addr;

    if (eg_probe_armed && eg_probe_data &&
        addr >= (uintptr_t)eg_probe_data &&
        addr < (uintptr_t)eg_probe_data + eg_probe_page_sz &&
        eg_emulate_load(uc, pc, addr)) {
        eg_fault_total++;
        if (eg_nfault < EG_MAX_FAULT_PCS)
            eg_fault_pcs[eg_nfault++] = pc;
        return;
    }

    /* Not ours (or a store / unsupported form): restore and re-raise. */
    eg_probe_bad++;
    eg_probe_armed = 0;
    sigaction(SIGSEGV, &eg_old_segv, NULL);
    raise(sig);
}

static EG_UNUSED void eg_probe_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = eg_segv_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &eg_old_segv) != 0) {
        perror("sigaction");
        exit(2);
    }
}

static EG_UNUSED void eg_probe_begin(eg_secure_buf *b)
{
    eg_secure_sync(b);
    eg_probe_data = b->data;
    eg_probe_shadow = b->shadow;
    eg_probe_page_sz = b->page;
    if (mprotect(b->data, b->page, PROT_NONE) != 0) {
        perror("mprotect");
        exit(2);
    }
    eg_probe_armed = 1;
}

static EG_UNUSED void eg_probe_end(eg_secure_buf *b)
{
    eg_probe_armed = 0;
    if (mprotect(b->data, b->page, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        exit(2);
    }
}

static int eg_cmp_uintptr(const void *a, const void *b)
{
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return (x > y) - (x < y);
}

/* Per-call count of sensitive-page accesses (one per emulated fault).  The
 * count is what distinguishes a sensitive PC that is used *only* for the
 * secret from one shared with other data (e.g. a byte-load helper used for
 * both operands): if the per-call sensitive-PC executions exceed this count,
 * some executions of those PCs carried non-secret bytes. */
static int eg_probe_percall[64];
static int eg_probe_npercalls;

static void eg_probe_note(int n)
{
    if (eg_probe_npercalls < 64)
        eg_probe_percall[eg_probe_npercalls++] = n;
}

#define EG_PROBE_CALL(BUF, EXPR)                                            \
    do {                                                                    \
        uint64_t _before = eg_fault_total;                                  \
        eg_probe_begin(BUF);                                                \
        EG_RUN_CALL(EXPR);                                                  \
        eg_probe_end(BUF);                                                  \
        eg_probe_note((int)(eg_fault_total - _before));                     \
    } while (0)

/* Print the deduplicated sorted fault-PC set: "probe_fault_pcs=0x..,0x..". */
static EG_UNUSED void eg_probe_report(const char *target)
{
    qsort(eg_fault_pcs, (size_t)eg_nfault, sizeof eg_fault_pcs[0], eg_cmp_uintptr);
    printf("probe target=%s faults=%llu pcs_seen=%d bad=%d pcs=", target,
           (unsigned long long)eg_fault_total, eg_nfault, eg_probe_bad);
    uintptr_t prev = 0;
    int first = 1;
    for (int i = 0; i < eg_nfault; i++) {
        if (!first && eg_fault_pcs[i] == prev)
            continue;
        printf("%s0x%lx", first ? "" : ",", (unsigned long)eg_fault_pcs[i]);
        prev = eg_fault_pcs[i];
        first = 0;
    }
    printf(" percall=");
    for (int i = 0; i < eg_probe_npercalls; i++)
        printf("%s%d", i ? "," : "", eg_probe_percall[i]);
    printf("\n");
    fflush(stdout);
}

#endif /* EG_PREVALENCE_HARNESS_H */
