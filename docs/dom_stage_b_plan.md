# Stage B: implement DoM, the exact value provider, and the larger-cache baseline

Goal: measure, with **IPC**, whether the committed-value provider recovers
Delay-on-Miss latency, and whether it matches a larger cache of the same area.

This is the implementation stage. Stage A only instrumented and profiled.

## Arms

| arm | cache | mitigation | area note |
|---|---|---|---|
| 1. baseline | L1 64 KiB, no L2 | none | reference |
| 2. DoM | L1 64 KiB | head-delay DoM | the cost |
| 3. DoM + provider | L1 64 KiB + value table | DoM + exact values | 4K entries ≈64 KiB; 65K ≈1 MiB |
| 4. DoM + larger cache | L1 1 MiB **or** L2 1 MiB | DoM | same area as the 1 MiB provider |
| 5. baseline + larger cache | L1 1 MiB / L2 1 MiB | none | cache null |

The decisive comparison is 3 (1 MiB provider) vs 4 (1 MiB cache): same area,
same DoM, different way to supply the value.

## DoM model (B1)

Simplest correct model first: **head delay**.

- A load that misses L1 while speculative is not sent to the cache.
- It is released when it becomes the ROB head (the v1 safe point).
- On squash, the delayed request is dropped; no state changed.
- Correctness: architectural equality with the baseline; the delayed load
  eventually performs the real access; no reads are served from the shadow.

Later refinement: release when all older branches are resolved (v2), as in
ISCA'19.

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

- 65K entries × ~16 B ≈ 1 MiB; 4K entries ≈ 64 KiB.
- Compare at equal total on-chip area:
  - 1 MiB provider + 64 KiB L1 vs 1 MiB L1 vs 1 MiB L2 + 64 KiB L1.
- Also report the 64 KiB provider, where the area is small.

## TDD layers

Each layer: write the test, show red, implement, green, mutation, regress.

- **B0** harness: a program that mixes loads/stores/branches/squashes/atomics;
  baseline/DoM equality; a timeout.
- **B1** delay + safe + release: the load is held on a speculative miss and
  released at the head; a microbench shows the expected IPC loss.
- **B2** squash: a delayed load in a mispredicted path is dropped, not served.
- **B3** provider: a store then a load hits the table; a store invalidates;
  an older in-flight store forwards (not the table); a permission-fault load
  is not served; a speculatively-filled value does not populate.
- **B4** larger cache: the config; architectural equality; the same workload.
- **B5** SPEC: the arms above; IPC; the comparison.

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

- IPC for arms 1–5 per workload;
- DoM cost = (1 − arm2/arm1);
- provider recovery = (arm3 − arm2)/(arm1 − arm2);
- provider vs larger cache at equal area = arm3 vs arm4;
- area, lookup latency, invalidation cost, security caveats.

## Validity

- probe/DoM off vs on: the baseline must be unchanged.
- deterministic traces/runs.
- the delayed set from Stage A predicts where the provider should help
  (omnetpp/xalancbmk positive; mcf/lbm negative).
- the larger-cache arm is the null hypothesis; if arm3 ≈ arm4, the provider
  is “just a cache” on this workload — that is a reportable result.
