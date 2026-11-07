# Stage A: valid coverage screen (TDD plan)

## Goal

Produce a **valid screen** for exact value providers under Delay-on-Miss, without
implementing DoM. Stage A answers:

1. What fraction of the **DoM-delayed set** can an exact provider serve?
2. Broken down by pattern class (zero, constant, affine-index, affine-value,
   pointer-affine, runs)?
3. With **infinite** and with **finite** descriptor budgets?
4. Weighted by a per-load **cost model**, not by miss count alone?

Stage A does **not** answer:

- the real IPC benefit (needs DoM + subset-oracle, Stage B);
- area, energy, lookup latency;
- whether the provider is worth building.

## Base

- Branch `feat/dom-stageA`, from `stable` = `c8222cc67a` (gem5 v25.1.0.1).
- No `src/cpu/lvp/`, no `src/mem/cache/shadow_*`, no LVP config.
- The old work stays on `feat/safespec-layer2`.

## Status

- **A0 done.** `feat/dom-stageA` from `stable` (`c8222cc67a`); no
  `src/cpu/lvp/`, no SafeSpec. Old work remains on `feat/safespec-layer2`.
- **A1 done.** `tools/dom_stage_a/analyzer.py`, 20 unit tests green
  (`python3 -m unittest discover -s tests/dom_stage_a -v`); both mutations
  caught (cost formula, committed-only population).
- **A2.1 (v0 probe) written.** `src/mem/cache/dom_stage_a.{hh,cc}`, env-gated
  by `GEM5_DOM_STAGE_A_TRACE`, logs L1-D load misses with their fill values
  plus committed stores. Smoke test: `tests/dom_stage_a/run_micro.sh`,
  `tests/test-progs/dom_stage_a/array_pattern.c`,
  `tests/dom_stage_a/check_trace.py`. Checker validated on a synthetic trace;
  a value mutation is caught.
- **v0 limits (deliberate).** Misses only, so it is a *lower bound* on the
  DoM-delayed set; no safe tick, no cost weighting; completed misses are
  treated as committed (optimistic). A2.2 adds provenance (fill sequence +
  commit watermark), A2.3 adds the safe point and cost model.
- **Real-workload scale check (mcf, local).** 5M -> 37 MB / 11 s; 50M ->
  341 MB / 97 s, 7.57M stores, 56,980 misses. Probe v0 per-word coverage on
  the 50M trace: 3.0% count / 3.6% weighted (1,701 hits, 568,603 unique
  words, zero tick-order violations). **Not representative:** that window is
  warm (IPC 1.93 vs 0.35 on the full 1B run, 0.11% vs ~1.9% miss rate), so
  this is a plumbing/scale check, not the Stage A number. 200M belongs on
  the server; the region (T3) oracle is still unmeasured.
- **Local build** `scons build/RISCV/gem5.opt -j2` in progress; the SPEC
  campaign runs on the server.

## Validity contract (what makes the number meaningful)

- **Set.** DoM-delayed loads =
  - an L1 miss while the load is speculative, **plus**
  - a hit on a line whose installing fill came from a load that was still
    speculative at the hit.
  - Implemented with a per-block `fillSeq` and the ROB's committed watermark.
    A squashed installing load invalidates the provenance (the line would not
    exist under DoM).
- **Weight.** Per delayed load, `cost = min(safe_tick - issue_tick,
  miss_latency)`; 0 for non-delayed loads. For a speculative-hit-turned-miss,
  use the installing fill's latency. This is a **ceiling per load**, not IPC.
- **Safe point.** v1: the load becomes the ROB head. v2: all older branches
  resolved (later; measure both).
- **Causality.** Offline replay uses only committed events with tick < query
  tick. No future stores, no future pattern samples. A pattern must be
  recognizable from past committed state (online, not hindsight).
- **Security.** Population from committed state only; report the delta against
  an (unsafe) speculative-population upper bound separately.
- **No product.** `coverage x all-oracle` is never reported as a benefit. The
  two numbers stay separate; the bridge is the Stage B subset-oracle.

## Layers, tests first

Each layer: write the tests, show **red**, implement, show **green**, run a
mutation check (deliberately break the rule; test must fail), then regress the
previous layers.

### A0 - skeleton (no gem5 code)

- `tools/dom_stage_a/` analyzer package.
- Trace schema v1 (below).
- `tests/dom_stage_a/` unittest suite.
- Tests: schema round-trip; empty trace; cost-model unit cases.
- Red: analyzer stubs raise.

### A1 - analyzer core (pure Python)

Tests first, synthetic traces with known answers:

| test | asserts |
|---|---|
| `test_cost_formula` | `min(slack, L)` for the four regimes (slack 0, slack < L, slack > L, hit) |
| `test_zero_pattern` | [0,0,0] -> `ZERO`, `value_at` = 0 |
| `test_constant_pattern` | [7,7,7] -> `CONSTANT`, value 7 |
| `test_affine_value` | [100,110,120] at base+0/4/8 -> `AFFINE_VALUE`, reconstructs all |
| `test_affine_index` | [0,1,2] -> `AFFINE_INDEX` |
| `test_pointer_affine` | [b, b+64, b+128] -> `POINTER_AFFINE` |
| `test_store_invalidates_word` | committed store to a word -> lookup miss |
| `test_store_invalidates_region` | deviating store inside a region -> region miss/split |
| `test_causal_order` | a store after the query is not visible |
| `test_committed_only` | a squashed load does not populate |
| `test_finite_capacity` | LRU eviction; coverage for the evicted entry = 0 |
| `test_weighted_coverage` | cover the cost-100 load of {100,1}: weighted 100/101, count 1/2 |
| `test_no_hindsight` | pattern entry created only after N committed samples; earlier load uncovered |
| `test_region_vs_word` | 1000-word affine array: 1 descriptor covers it, 64-entry word table does not |

Implementation: `PatternKind`, `Pattern.value_at`, `detect_pattern`,
`dom_cost`, `PerWordOracle`, `RegionTable`, `coverage`.

### A2 - gem5 probe (O3)

Tests first: microbenchmarks under `tests/test-progs/dom_stage_a/`, plus a
runner that reads the trace and asserts.

| test | asserts |
|---|---|
| `mb_miss` | a speculative L1 miss appears with `set=miss` |
| `mb_spec_hit` | a hit on a still-speculative line appears with `set=spec-hit` |
| `mb_committed_hit` | the second load to a committed line is **not** in the set |
| `mb_squash` | a squashed installing load does not make the line safe |
| `mb_safe_head` | `safe_tick` is recorded when the load reaches the ROB head |
| `mb_fields` | timestamps monotonic; `addr`, `pc`, `value` match the program |
| `mb_determinism` | two runs produce identical traces |
| `mb_probe_off` | with the env gate off, the run's IPC/stats match the baseline |
| `mb_stats` | counts add up (delayed = misses + spec-hits) |

Instrumentation:

- `CacheBlk`: `fillSeq` (installing load's seq), `fillLatency`.
- `Request`: `specSeq`, `commitSeq` (set by the LSQ at issue).
- `LSQUnit::executeLoad`: record `issue_tick`; set the request fields.
- `ROB`/`commit`: expose the committed watermark; record `safe_tick` when the
  load becomes head (v1) or when all older branches resolve (v2).
- Fill path: set `fillSeq` on install; deliver the value normally.
- Squash path: invalidate the provenance of the squashed load's fill.
- Trace writer: env `GEM5_DOM_STAGE_A_TRACE=<path>`; write schema v1.
- Stats: `domStageA.{delayed,misses,specHits,costSum}`.

No `if (false && ...)` debug gates: everything behind a runtime env var.

### A3 - integrate

Run the microbenchmarks, feed the traces to the analyzer, assert the expected
coverage/set membership. Mutation: break the safe point or the provenance rule;
the integration test must fail.

### A4 - SPEC subset

- Memory-bound first: 505.mcf, 519.lbm, 531.deepsjeng, 557.xz, 503.bwaves,
  541.leela, 523.xalancbmk (if it runs under SE) + 1-2 compute controls.
- Warmup, then a fixed window (e.g. 200 M instructions; SimPoints if broadened).
- Per benchmark, not aggregate; report mcf and lbm as the two poles.
- Output: coverage by class, infinite vs finite, count-weighted and cost-weighted.

### A5 - report and gate

- Two separate numbers: coverage of the delayed set; all-oracle cost ceiling.
- Gate: cost-weighted compressed coverage at a realistic descriptor count vs
  the recompute reference (ISER 43% average coverage). If the compressed ceiling
  is < ~10% cost-weighted, pivot; if it is high but finite descriptors collapse,
  the problem is recognition/splitting; if it is high, Stage B.

## Trace schema v1

Text (microbenchmarks) and a compact binary writer (SPEC):

```
L <seq> <tid> <pc> <addr> <size> <value> <issue> <safe> <latency> <set> <committed> <fillSeq> <commitSeq>
S <seq> <tid> <addr> <size> <value> <commit>
```

- `set` in `{miss, spec-hit}`.
- `committed` in `{0,1}`; squashed loads are emitted with 0 and never populate.
- ticks are gem5 ticks; the analyzer is tick-agnostic.
- Only delayed loads and all committed stores are written (keeps the trace
  small); the analyzer reconstructs committed state from stores + committed
  delayed loads.

## SPEC run protocol

```
scons build/RISCV/gem5.opt -j2          # OOM-safe
GEM5_DOM_STAGE_A_TRACE=/tmp/dom/<bench>.trace \
  build/RISCV/gem5.opt -d /tmp/dom/<bench> ... --maxinsts=<window>
```

Pinned to a P-core; one benchmark at a time; record the exact command line.

## Definition of done for Stage A

- Analyzer unit tests green; mutation checks shown.
- Microbenchmark integration tests green; probe-off run unchanged.
- One SPEC table (coverage vs descriptor count, by class) and the two separate
  summary numbers.
- A written decision: build T2 (per-word cache), build T3 (compression), or
  pivot. No performance claim is made in Stage A.
