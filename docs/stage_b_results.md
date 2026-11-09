# Stage B results: Delay-on-Miss B1-B3

Branch: `feat/dom`.  Last updated: 2026-11-09.

## What is implemented

### B1: shadow buffer, C-shadow, shadowed-miss drop/re-issue

- Per-thread shadow buffer (`src/cpu/o3/dom.hh`) in dispatch order.
- Conditional and indirect control instructions cast a C-shadow; direct
  unconditional jumps are already resolved at decode.
- At ROB insertion a load records the youngest unresolved caster; this is its
  shadow stamp (`DynInst::domCaster`).
- At load issue (`LSQUnit::read`, after store-forwarding checks) a shadowed
  load probes the L1 with a side-effect-free tag lookup.  A hit proceeds; a
  miss is dropped (no MSHR, no cache state change, no packet) and re-issued
  through the normal load path after release, so store forwarding is rechecked.
- Squash discards SB entries younger than the squash point; delayed loads in
  the wrong path are discarded by the normal LSQ squash.
- Runtime gate: `GEM5_DOM`.

### B2: deferred replacement side effects on shadowed hits

- A shadowed demand hit uses `BaseTags::probeBlock`, which locates the block
  without touching the replacement policy or the reference count.
- `BaseCache` records the hit as `{caster, block address, secure, load seq}`
  in `domDeferredHits` and increments `domHitsDeferred`.
- When the caster retires, `BaseCache::domRelease` applies the deferred
  replacement update (`BaseTags::touchBlock`) and increments
  `domHitsReleased`.  If the block has been evicted, the update is dropped.
- `BaseCache::domSquash` drops deferred entries in a squashed path.
- Prefetcher notifications are not deferred in this milestone; the L1
  replacement update is.

### B3: E-shadows and the retirement release point

- Every data memory access (load/store/atomic, excluding prefetches) casts an
  E-shadow.  The cast is cleared when its translation completes (fault or
  no-fault) in `LSQRequest::finish`; the entry leaves the SB at retirement.
- Stores therefore also cover the paper's D-shadow for unresolved store
  addresses.  The M-shadow (TSO load-load order) is not applicable to the
  RISC-V configuration.
- **Release point:** verification clears the `casts` bit, but a load that
  already recorded a caster stays shadowed until that caster **retires and
  leaves the SB** (`DomShadowBuffer::contains`).  In-order retirement
  guarantees every older caster has also retired by then.  Releasing at
  verification instead of retirement would let a load issue before an older
  unresolved caster, defeating the speculative-cache-state guarantee.
- Stats: `domCastersC`, `domCastersE`, `domDelayedMisses`, `domReleased`.

### B5: exact committed-value provider

- `DomValueProvider` (`src/cpu/o3/dom_provider.hh`): 4-way set-associative
  per-word table with byte granularity.  Each entry stores a tag, a 64-bit
  value, and a byte mask of known committed bytes.  A lookup succeeds only
  when every requested byte is known.
- Queried in `LSQUnit::read` on a shadowed L1 miss, **after** the store
  forwarding scan and **before** the DoM drop.  A hit fills `memData` and
  schedules a fixed-latency writeback (`GEM5_PROV_LATENCY`, default 1 cycle),
  skipping the memory access; a miss falls through to the normal drop/re-issue.
- Populated only from committed accesses in `Commit::commitHead`: loads fill
  the loaded bytes; unmasked stores fill the **store-queue copy** of the data
  (`DynInst::memData` is not populated for stores).  Masked, atomic,
  store-conditional, cache-block-zero/`STORE_NO_DATA`,
  cache-maintenance, local, uncacheable, cross-word, and otherwise
  unrepresentable writes invalidate every affected word.  Prefetches do not
  fill, and non-cacheable/local/maintenance requests are never served.
- Enable with `GEM5_PROV`; size with `GEM5_PROV_ENTRIES` (default 65536;
  4096 for the 64 KiB arm).  Stats: `domProvHits`, `domProvMisses`,
  `domProvFills`, `domProvFlushes`.
- Older in-flight stores forward first because the LSQ forwarding scan runs
  before the provider lookup; a faulting/untranslated access never reaches
  `read`.
- Eviction bug found by the mcf smoke run: a reused provider way kept the
  previous word's value/mask, so a partial fill could serve stale bytes and
  the 4K arm aborted mcf with `read error`.  `fill` now clears the entry when
  it does not already hold the word.  `test_provider.cc` forces a set
  collision, fills a fifth word partially, and requires the wide lookup to
  miss; removing the reset makes that test fail.  A provider hit also refuses
  to bypass an older store whose address is still unresolved or whose data is
  not yet available.
- Syscall/DMA coherence: `read`/`write`/`mmap`/`mprotect` can write guest
  memory without committed stores.  The common `Process::syscall` path (used
  by every ISA's `EmuLinux::syscall` before the syscall is emulated) calls
  `BaseCPU::domProviderFlush`, and the RISC-V TLB flushes after a successful
  page-fault fixup.  This was found by the mcf smoke run: a load filled the
  input-buffer word, then the kernel's `read()` rewrote it, and without the
  flush the 4K arm served old text and aborted.
- Debug aids (env-gated): `GEM5_PROV_CHECK` performs a functional read on
  each provider hit and panics on a mismatch; `GEM5_PROV_TRACE` records
  fill/invalidate/flush/lookup-hit events.  The stale mcf hit was found with
  these.
- Known limitations:
  - single-core: only the CPU that takes the syscall/page fault flushes its
    provider; a multi-CPU configuration would need all providers flushed.
  - device DMA that does not go through a syscall is not covered; the
    current L1-only configuration has no such device.
  - prefetcher side effects on shadowed hits are not deferred (B2 defers the
    replacement policy only).

## Tests

Host-side unit tests (no gem5 build):

```
bash tests/dom_stage_b/run_unit.sh
shadow buffer: all checks passed
provider: all checks passed
```

- `test_shadow_buffer.cc` covers add/resolve/retire/squash, the
  `isCasting`-vs-`contains` distinction, and the **self-shadow trap**: adding a
  load's own E entry before assigning its caster makes `youngestCaster()`
  return the load itself.  This is the bug that hung the first B3 run.
- Mutation checks: never clearing the cast bit aborts the shadow-buffer test;
  ignoring the provider's byte mask aborts the provider test.
- `test_provider.cc` covers full/partial fills, byte updates, invalidations,
  straddling accesses, round-robin eviction, **way reuse after eviction**
  (the stale-mask leak that broke mcf), cross-word invalidation, and
  **syscall/DMA flush**.

Gem5 tests (hard timeout per run; a hang is a failure):

| test | result |
|---|---|
| `run_b0.sh` branch-shadow miss | base 0.195579, DoM 0.177929 IPC; delayed 480, released 122; output equal |
| `run_b2.sh` shadowed hit | deferred 1976, released 997; output equal |
| `run_e.sh` TLB-thrash E-shadow | E-casters 13531, delayed 2503; output equal |
| `run_correctness.sh` branchy/squash | outputs equal; 4597/4241 and 14814/14437 delayed/released |
| `run_provider.sh` provider hit | x=42, y=42; prov hits 23560, misses 218; functional check clean |
| `run_provider.sh` provider update | x=99, y=99; prov hits 2163, misses 223; functional check clean |
| `run_pagefault.sh` | mmap/unmap/remap output equal; provider check clean |
| `run_stress.sh` load chain | output equal; IPC ratio 0.996; delayed 517; completes |
| `run_stress.sh` hit loop | output equal; IPC ratio 0.999; no hit penalty |

`load_chain` is a serial pointer chase over a 1 MiB array; a self-shadow or a
missed release would hang it.  `hit_loop` is an all-hit kernel; a bug that
delays hits would tank its IPC.  Every runner wraps gem5 in
`timeout "${GEM5_TIMEOUT:-180}"`.

Run everything:

```
GEM5_TIMEOUT=120 bash tests/dom_stage_b/run_all.sh
all Stage B micro tests passed
```

## Invariants added

`panic_if` checks (always active, not debug-only) in:

- `CPU::domInsert`: a load's caster must be `< load_seq` (never itself or a
  younger instruction).
- `CPU::domShadowed`: same check on every shadow query.
- `LSQUnit::releaseShadowed`: a delayed/released load's caster must be older.
- `LSQUnit::read`: a load must not be delayed without a caster.

## Not yet implemented

- B6: equal-area larger-cache arm (runner supports `--l1d_size=128KiB/1MiB`;
  SPEC runs pending).
- B7: SPEC IPC arms and the provider-vs-cache comparison.
- Prefetch side-effect deferral on shadowed hits (replacement only for now).

## SPEC smoke (mcf test, 2M-instruction window)

Preliminary, not the final ref 200M comparison.  All arms commit exactly
2,000,006 instructions and produce the same architectural path.

| arm | IPC |
|---|---|
| base (64 KiB L1) | 1.323501 |
| DoM | 1.296136 |
| DoM + 4K provider | 1.310706 |
| DoM + 65K provider | 1.311108 |
| DoM, 128 KiB L1 | 1.291995 |
| DoM, 1 MiB L1 | 1.384378 |
| base, 1 MiB L1 | 1.407715 |

- DoM cost: ~2.1%.
- Provider recovery: ~53% (4K) and ~55% (65K) of the DoM cost.
- Equal-area comparison on mcf: the 1 MiB L1 (1.3844) beats the 65K
  provider (1.3111); the 4K provider (1.3107) beats the 128 KiB L1
  (1.2920).  This is the mcf-negative result expected from Stage A: a
  larger cache wins on this workload, while the provider still helps
  relative to the DoM-only arm.
- These are test-input, short-window numbers; the ref 200M runs on the
  server are the decisive measurement.
