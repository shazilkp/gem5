# Response to the independent peer review (R1–R8)

The independent audit's eight refutals were checked against code and paper.
Status: **R1, R2, R3, R4 (partial), R5, R6, R7, R8 addressed**; remaining items
are stated limitations, not hidden claims.

| # | disposition | fix / evidence |
|---|---|---|
| R1 | fixed | `commitLoad` uses the `SpecGVQ` distance/diff snapshot captured when a load **predicts** (tag match, `ctr==7`); unpredicted training loads keep the live table. Commit `6e6fcd4d8d`. |
| R2 | fixed (approximation) | lastMisp is a 1024 **instruction-dispatched-after-the-squash** countdown, not `fetchSeqNum+1024`; later wrong-path instructions can still consume slots before their own squash. Suppression stat counts only would-predict loads. Commit `5a81c3ed1b`. |
| R3 | fixed | normal `+3` is modelled with a pending-publish stage: `DynInst` pending flag, IQ marks ready only when `lvpPredictionAvailable()`, IEW pending map serviced in `tick()`, cancel on commit/squash. Deferred `+2` publishes through the same stage. Commit `c301573861`. |
| R4 | fixed (prefetch) + scoped | zero-destination/prefetch-like loads are skipped in dispatch/completion/commit; LQ=32 is explicit; subword sign/zero extension is a stated gap. Commit `0c406768a3`. |
| R5 | measured + split | strict A4 (`EGDIFF_NVQ_BASE=0`) recovers 8/8 with `egdiffNVQBases=0` and `base=specgvq`; strict SipHash fails 0/16, so its 16/16 is an **NVQ-extension** result. The main A4 claim is paper-faithful; SipHash is extension-only. |
| R6 | measured | varying the guess on every A4 attempt still recovers 8/8; the shared `load64_le` PC does not lock. The narrow-target/ABI limitations remain. Commit `59d3bc9056`. |
| R7 | fixed | Mode 2 was missing `flush_drain()` after `flush_l1()`; with it the off control is `known=00`, FLOP emits its own 0 (`known=ff`, `NO-MATCH`), and the gate requires both. The §5/§9.5 contradiction is removed. Commit `5f8d69d4be`. |
| R8 | corrected claim | F2 is a first-30M-from-`_start` **smoke test**, not conformance. `505.mcf`'s 64.51% is startup; the paper's Fig. 10(b) is <10%. The 22.9% cherry-pick is withdrawn; deepsjeng is reported; config deviations are stated. Commit `b967ed66d4`. |

## R1 details

At dispatch, only a load that actually attempts a prediction (tag match,
`ctr==7`) snapshots `distance`/`diff` into its queue entry. Commit compares the
actual stride against that snapshot. Applying the snapshot to unpredicted
training loads starved confidence growth and broke Layer 2 (no induced
misprediction); the narrow form preserves training.

## R2 details

`suppressUntil = fetchSeqNum + 1024` was wrong because gem5 assigns fetch
sequence numbers before squash. The window now arms at `squashFrom()` with
`instructionCount + 1024`; `dispatchInstruction()` counts every correctly
dispatched instruction. A deferred publish for a load dispatched **before** the
mismatch is allowed; the window suppresses new predictions.

## R3 details

The reviewer's root cause was correct: `InstructionQueue::addToProducers` marks
a destination ready whenever `lvpPredicted()` is true, so a delayed PRF write
left consumers reading stale registers. The model now keeps `_lvpPredictionPending`
until the publish event; latency 0 keeps the old immediate path. Trace:
`schedule-publish ... at T+1500 ticks (+3)` then `publish ... at T+1500`.
Matrix (0/0, 3/0, 0/2, 3/2, 5/2) all MATCH 8/8; Layer 4 17/17 and Layer 6
36/36.

## R4 details

Remaining gaps: predicted values are not sign/zero-extended for `lb`/`lh`/`lw`
(the access size is unknown at dispatch), and the queue is capped at the paper's
LQ=32; LQ=48/64 configurations are not modelled. The PoCs use `ld`/`lbu` with
`diff=0`, so their results are unaffected.

## R5/R6 details

The audit's warm-up and constant-input objections are real concerns, but the
proposed failures did **not reproduce** in this model. Cold A4 (no
`target_verify32_selfcheck`, no two pre-calls) recovers 8/8; varying the guess
per attempt recovers 8/8; cold SipHash recovers 16/16. The shared `load64_le`
PC alternates key/guess values and does not lock, so the victim does not
self-mispredict. These are narrow controls (one key word, zero-spill static
ABI, empty-message SipHash), not a full realistic workload. The real-target
mechanism is GVQ/NVQ, not pure in-flight SpecGVQ.

## R7 details

Before the fix: `EGDIFF_ON=0` emitted `3f3f…00` (`known=fe`) and FLOP emitted
`0` (`known=ff`) because the eviction's outstanding MSHRs biased the scan.
`flush_drain()` before the replay fixes the off control. FLOP's zero is a real
FLOP prediction, not a fabrication; it is `NO-MATCH`. `run_layer4.sh` now gates
Mode 2 off on `known=00` and on on `known=ff`.

## R8 details

`run_spec.py` runs 30M from `_start`; the paper uses 50M warm-up + 100M
SimPoints. `505.mcf`'s 64.51% is startup array initialisation; the paper's
Fig. 10(b) is below 10%. The former 22.9% “inside the band” claim is withdrawn.
All five attempted benchmarks are reported; the SPEC result is a smoke test.
Configuration also differs: TournamentBP vs L-TAGE, 256/256 vs 168/168
registers, no L3/prefetcher, 0 latency.

## Remaining limitations

- No real capability boundary (same-process non-disclosure convention).
- No hardware validation; the cache observation is gem5.
- Mode 2 is a crafted-victim conformance experiment, not a real-target attack.
- One key word (A4), empty-message SipHash; no full key.
- Subword extension and LQ>32 are not modelled.
- No differential co-simulation against `egdiff_sim.py`.
- Defence evaluation (SafeSpec/DoM) is deferred.

Gate totals after the fixes: Layer 1 17, Layer 2 10, Layer 3 13, Layer 4 17,
Layer 5 24, Layer 6 36 = **117**.
