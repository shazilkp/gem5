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
- `test_provider.cc` covers full/partial fills, byte updates, straddling
  accesses, and round-robin eviction.

Gem5 tests (hard timeout per run; a hang is a failure):

| test | result |
|---|---|
| `run_b0.sh` branch-shadow miss | base 0.195579, DoM 0.177929 IPC; delayed 480, released 122; output equal |
| `run_b2.sh` shadowed hit | deferred 1976, released 997; output equal |
| `run_e.sh` TLB-thrash E-shadow | E-casters 13531, delayed 2503; output equal |
| `run_correctness.sh` branchy/squash | outputs equal; 4597/4241 and 14814/14437 delayed/released |
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

- B5: committed-value provider integration (unit model and tests exist).
- B6: equal-area larger-cache arm.
- B7: SPEC IPC arms and the provider-vs-cache comparison.
- Prefetch side-effect deferral on shadowed hits (replacement only for now).
