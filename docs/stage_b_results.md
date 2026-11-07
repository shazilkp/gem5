# Stage B results: Delay-on-Miss B1

Branch: `feat/dom`.  Date: 2026-11-08.

## What B1 implements

Proper (ISCA'19) Delay-on-Miss, C-shadow only:

- Per-thread shadow buffer (`src/cpu/o3/dom.hh`) in dispatch order.  Conditional
  and indirect control instructions cast a C-shadow; direct unconditional jumps
  are already resolved at decode.
- At ROB insertion a load records the youngest unresolved caster; this is its
  shadow stamp (`DynInst::domCaster`).
- At load issue (`LSQUnit::read`, after the store-forwarding checks) a shadowed
  load probes the L1 with a side-effect-free tag lookup
  (`BaseCache::inCache`).  A hit proceeds normally.  A miss is **dropped**: no
  MSHR, no cache state change, no packet sent.
- The C-shadow is lifted at branch verification (`IEW::executeInsts`, after the
  misprediction check); `commitHead` keeps a fallback.  At release the delayed
  load is re-issued through the normal load path (`retryMemInst`), so store
  forwarding is rechecked rather than replaying a stale packet.
- Squash drops shadow-buffer entries younger than the squash point; delayed
  loads in the squashed path are discarded by the normal LSQ squash.
- Runtime gate: `GEM5_DOM` in the environment.  The same binary runs both arms.
- Stats: `domShadowedLoads`, `domDelayedMisses`, `domReleased` in each
  `system.cpu.lsqN`.

Not in B1: deferred hit side effects (LRU/prefetch, B2), E/D/M shadows (B3),
value provider (B5), larger-cache arm (B6), SPEC (B7).

## Tests and evidence

### Unit: shadow buffer (`tests/dom_stage_b/test_shadow_buffer.cc`)

```
bash tests/dom_stage_b/run_unit.sh
shadow buffer: all checks passed
```

Mutation check: setting `entry.casts = true` in `DomShadowBuffer::resolve`
(never clearing the cast bit) makes the unit test abort at
`!sb.resolve(20)`; the mutation was reverted.

### B0 integration: branch-shadow microbenchmark

```
bash tests/dom_stage_b/run_b0.sh
base_ipc=0.195579 dom_ipc=0.187618 equal_output=True
dom_delayed=294 dom_released=55
```

The DoM arm is architecturally identical and slower on the shadowed miss, and
the delayed/release counters prove the path was exercised.  Before B1 the same
test was red: `GEM5_DOM` had no effect and the IPC was identical.

### Correctness regression: branchy and mispredicting kernels

```
bash tests/dom_stage_b/run_correctness.sh
ok array_branch_base: output equal; delayed=322 released=66
ok squash_shadow_base: output equal; delayed=8109 released=7845
```

`array_branch` mixes loads, stores, and data-dependent branches.
`squash_shadow` has two unpredictable branches over a 512 KiB array; the gap
between delayed (8109) and released (7845) is delayed loads discarded by
misprediction squash.  Both outputs are byte-identical to the baseline.

## Notes / caveats

- B1 drops shadowed misses and re-issues them; it does **not** yet defer the
  LRU/prefetch side effects of a shadowed hit (B2).  The comparison in this
  milestone is therefore a lower bound on DoM's cache-side disturbance.
- The shadow is lifted at execute-time verification.  This is the paper's
  semantics; an earlier commit-time-only version was more conservative and
  over-delayed loads (B0 DoM IPC 0.1838 vs 0.1876 at execute-time).
- The cache is reached through the bound data port
  (`RequestPort::getResponsePort` + `BaseCache::CpuSidePort::getCache`).  This
  avoids a new SimObject parameter and works for the L1-only configuration.

## Next milestones

1. B2: shadowed-hit LRU/prefetch deferral, with a test that a shadowed hit does
   not change the replacement state until release.
2. B3: E/D/M shadows.
3. B5: per-word committed-value provider.
4. B6/B7: equal-area larger cache and SPEC IPC.
