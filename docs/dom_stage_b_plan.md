# Stage B: implement DoM, the exact value provider, and the larger-cache baseline

Goal: measure, with **IPC**, whether the committed-value provider recovers
Delay-on-Miss latency, and whether it matches a larger cache of the same area.

This is the implementation stage. Stage A only instrumented and profiled.

## Arms

| arm | cache | mitigation | area note |
|---|---|---|---|
| 1. baseline | L1 64 KiB, no L2 | none | reference |
| 2. DoM | L1 64 KiB | proper DoM | the cost |
| 3. DoM + provider | L1 64 KiB + 4K-entry value table | DoM + exact values | ~64 KiB extra |
| 4. DoM + larger L1 | L1 128 KiB | DoM | 64 KiB extra |
| 5. DoM + L2 | L1 64 KiB + L2 64 KiB | DoM | 64 KiB extra |
| 6. baseline + larger cache | L1 128 KiB or L2 64 KiB | none | cache null |

The decisive comparison is 3 vs 4 and 3 vs 5: the same 64 KiB of extra
area spent on the provider, on L1 capacity, or on L2 capacity.  The
larger 65K-entry provider and 1 MiB L1 arms remain in the repository only
as out-of-scope sensitivity points; a provider larger than the L1 is not
a plausible design and must not be used as a research signal.

## DoM model (B1/B2/B3)

Implement the paper's **delay-on-miss** (third version), not the naive/eager
baselines:

- **Shadow buffer (SB):** circular, one entry per ROB instruction, in dispatch
  order; a "casts" bit is cleared when the instruction no longer casts; an
  entry is removed only at the head. Track the shadow types: C (branches),
  E (memory/arithmetic exceptions), D (stores with unresolved addresses),
  M (TSO load-load order). Start with C+E, then add D/M.
- **Release queue:** one entry per load; record the youngest shadow-casting SB
  entry at the load's dispatch (the shadow stamp). The load is released when
  that SB entry exits the SB.
- **Shadowed L1 hit:** the cache returns the data, but the visible side
  effects (replacement/LRU, prefetch) are delayed. On release the CPU signals
  the cache to perform them.
- **Shadowed L1 miss:** the request is **dropped** (a "shadowed L1 miss"); no
  MSHR allocation, no cache state change. On release the load re-issues the
  request and the normal miss path runs.
- **Squash:** the load's release-queue entry and any pending re-issue are
  dropped; nothing was allocated.
- **VP/provider:** optional. In Stage B the exact value provider replaces the
  VP; it is queried on the shadowed L1 miss before the drop, and the release
  path is skipped when it serves.
- **Correctness:** architectural equality; a hit's data is exact; a miss's
  data arrives after release; no speculative cache state.

This is the piece that must be right before any provider measurement: a hit
must not update the LRU, a miss must not allocate, and the release must do the
deferred work.

## Provider (B4)

Per-word committed-value table, populated by committed stores and committed
loads, invalidated by committed stores.

- Lookup **before** the DoM delay, on the speculative L1 miss.
- Hit → supply the value with a fixed latency; the load and its dependents
  proceed; no memory access.
- Miss → fall through to DoM.
- Populate only from committed state (no speculative fill).
- Invalidate on every committed store; forward from older in-flight stores
  (the store queue) before trusting the table.
- Permission/translation check before the lookup; per-context tags.
- Fixed hit latency; document the timing/side-channel argument.

## Area accounting (B5)

- 4K entries × ~16 B ≈ 64 KiB.  This is the realistic provider size.
- Compare at equal total on-chip area:
  - 4K provider + 64 KiB L1 vs 128 KiB L1 vs 64 KiB L2 + 64 KiB L1.
- The 65K-entry (~1 MiB) provider and 1 MiB L1 are out-of-scope sensitivity
  points only; do not use them as a valid design signal.

## TDD layers

Each layer: write the test, show red, implement, green, mutation, regress.

- **B0** harness: a program that mixes loads/stores/branches/squashes/atomics;
  baseline/DoM equality; a timeout.
- **B1** SB + release queue + C-shadow: a load after an unresolved branch is
  held; a shadowed L1 miss is dropped and re-issued after release; a hit
  returns data.
- **B2** hit side-effect delay: a shadowed hit returns data but the LRU is not
  updated until the release; the release updates it.
- **B3** E/D/M shadows: older memory/arithmetic (E), unresolved store (D),
  TSO load-load (M); each releases at the correct point.
- **B4** squash: a delayed load in a mispredicted path is dropped, not served;
  no MSHR, no LRU, no release entry leaks.
- **B5** provider: a store then a load hits the table; a store invalidates;
  an older in-flight store forwards (not the table); a permission-fault load
  is not served; a speculatively-filled value does not populate.
- **B6** larger cache: the config; architectural equality; the same workload.
- **B7** SPEC: the arms above; IPC; the comparison.

## Tests first

Microbenchmarks under `tests/test-progs/dom_stage_b/`:

- `baseline_eq.c` — architectural equality with/without DoM.
- `dom_delay.c` — a dependent chain where the delay is visible.
- `prov_hit.c` — a store, then a load that should be served by the table.
- `prov_inval.c` — a store after the entry; the load must see the new value.
- `prov_fwd.c` — a load younger than an in-flight store; forwarding wins.
- `prov_squash.c` — a delayed load in a wrong path is dropped.
- `prov_perm.c` — a load that faults is never served from the table.

## Experiments

SPEC 50M first (omnetpp, mcf, lbm, xalancbmk if available, leela, exchange2,
deepsjeng); then 200M for the interesting arms.

Report:

- IPC for the primary arms (baseline, DoM, 4K provider, equal-area L1,
  equal-area L2, and their no-DoM baselines) per workload;
- DoM cost = (1 − arm2/arm1);
- provider recovery = (arm3 − arm2)/(arm1 − arm2);
- provider vs equal-area L1 = arm3 vs arm4;
- provider vs equal-area L2 = arm3 vs arm5;
- area, lookup latency, invalidation cost, security caveats.

The 65K provider and 1 MiB L1 arms are out-of-scope sensitivity points.
They may be reported in an appendix only, never as a valid equal-area
research signal.

## Validity

- probe/DoM off vs on: the baseline must be unchanged.
- deterministic traces/runs.
- the delayed set from Stage A predicts where the provider should help
  (omnetpp/xalancbmk positive; mcf/lbm negative).
- the equal-area L1 and L2 arms are the null hypothesis; if arm3 ≈ arm4 or
  arm5, the provider is “just a cache” on this workload — that is a
  reportable result.  The 65K/1 MiB rows are not part of this test.
