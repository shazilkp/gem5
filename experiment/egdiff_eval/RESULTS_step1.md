# Step 1 — Mode 1 measurement robustness

Scope: the in-flight (`SpecGVQ`) measurement harness,
`tests/test-progs/egdiff_attack/poc_v1_specgvq_1shot.c`. The modelled hardware
and the mechanism are unchanged; this note is only about whether a byte the
harness reports was actually **measured** or was an artefact of the estimator.

## Symptom

The original harness voted for the single fastest of 256 probe lines per
attempt. It recovered its default value deterministically, but when the measured
value was changed, individual bytes were sometimes wrong, and the failing byte
moved with the value:

- `0x1122334455667788` → `…7716` (byte 0 wrong, once)
- `0xDEADBEEFCAFEBABE` → `c1adbeefcafebabe` (byte 7 wrong)
- an earlier rotation experiment produced `c1c1c1c1…`

Trying to fix this by rotating the probe index, averaging latencies, or adding a
warm-up pass moved the artefact or made things worse.

## Root causes

**1. The minimum of 256 cold misses is not noise-free.** When the speculative
probe fill does *not* happen, all 256 lines are cold misses and the "fastest"
line is a draw from the miss-latency distribution. That minimum is biased: it
lands on the scan's second line almost every time, so a byte with no leak was
reported as `0x01` (and as `0xC1` once the old code applied a fixed rotation).
That is exactly the `c1…`/`…16` family of failures: bytes with no successful
measurement were being fabricated from an empty vote.

Measured in gem5 (deterministic):
- speculatively filled line: ~14 cycles
- cold (evicted) line: ~114–142 cycles, mean ~120
- architectural (re-executed) sentinel line: ~14 cycles, every attempt

**2. MSHR starvation could silently remove the leak.** The eviction loop is a
stream of independent loads, so the out-of-order core starts the attack while
the last eviction misses are still outstanding. Those misses occupy all L1D
MSHRs, so the transient probe request cannot be issued before the squash
arrives and the line is never filled. The leak then disappears in a
scheduling-dependent way: 1/128 attempts leaked without a drain, 127/128 with
one. A `fence.i` between the eviction and the helper call removes the race. It
sits *outside* the transient window, so it does not change the mechanism — it
only guarantees that "probe area is cold" is actually true.

## Estimator

Replaced the naive vote with a calibrated one:

1. **Per-attempt calibration.** The architectural (post-squash) probe fills the
   sentinel line, so that line is a known-hot reference. With the mean cold
   latency of the other 255 lines, the attempt threshold is
   `(t_arch + t_cold) / 2`. If `t_arch * 4 >= t_cold` the hot/cold gap has
   collapsed (cache not cold, or the suffix did not re-execute) and the attempt
   is discarded as uninformative.
2. **Exactly one clear hit.** A vote is cast only when exactly one non-sentinel
   line is below the threshold. Zero fast lines = "no leak this attempt"; two or
   more = "ambiguous"; both cast no vote.
3. **Alternating sentinels.** The architectural sentinel alternates between
   `0xA5A5A5A5A5A5A5A5` and `0x5A5A5A5A5A5A5A5A`, whose bytes differ at every
   position. So a measured byte equal to the sentinel byte is still measured on
   the attempts that use the other sentinel. (The previous harness excluded one
   line globally, which made any byte equal to it structurally unrecoverable.)
4. **No fabrication.** A byte is accepted only with at least `MIN_VOTES = 2`
   votes and at least twice the runner-up's count. Otherwise it is reported as
   `UNKNOWN` and its `known` bit is cleared. The final verdict requires
   `known == 0xff`, so a partially measured value can never be reported as a
   success.

`REPS` defaults to 16 attempts per byte (8 per sentinel), so a byte whose value
equals one sentinel still gets 8 informative attempts.

## Results

Each row is one gem5 run of the final binary (the measurement is deterministic;
repeated runs of the same value reproduce the same output).

| measured value | result | `known` | note |
|---|---|---|---|
| `0x1122334455667788` | MATCH | `ff` | default |
| `0xDEADBEEFCAFEBABE` | MATCH | `ff` | previously `c1adbeef…` |
| `0xFFFF0000FFFF0000` | MATCH | `ff` | previously `…ff16` |
| `0xA5A5A5A55A5A5A5A` | MATCH | `ff` | contains both sentinel bytes |
| `0x5A5A5A5AA5A5A5A5` | MATCH | `ff` | contains both sentinel bytes |
| `0x1122334455667788` (EgDiff off) | NO-MATCH | `00` | 0 votes, all `UNDECIDED` |
| `0x1122334455667788` (FLOP LVP) | NO-MATCH | `00` | 0 votes |

Per-byte evidence for `0xA5A5A5A55A5A5A5A`: every byte has
`votes=8 runnerup=0 voted=8 sentinel_only=8 ambiguous=0 invalid=0`. The 8
`sentinel_only` attempts are exactly the half where the sentinel byte equals the
secret byte for that position; the other 8 carry a clear vote.

Disabled control, same binary: every byte is
`UNKNOWN … voted=0 sentinel_only=16`, i.e. only the architectural line was fast
and nothing was fabricated.

`run_layer4.sh` (`ONLY=1`, Mode 1) passes 7/7, including new gates requiring
`known=ff` on the EgDiff run and `known=00` on both controls.

## Reproduction

```bash
# one value, one run
riscv64-linux-gnu-gcc -static -O2 -fno-pic -no-pie \
  -DSECRET_VAL=0xA5A5A5A55A5A5A5AULL \
  -o /tmp/poc1 tests/test-progs/egdiff_attack/poc_v1_specgvq_1shot.c
EGDIFF_TYPE=egdiff EGDIFF_ON=1 build/RISCV/gem5.opt -d /tmp/r1 \
  configs/example/egdiff.py --cpu-type=O3CPU --caches --cmd=/tmp/poc1

# full sweep + controls
tests/test-progs/egdiff_attack/sweep_v1.sh          # RUNS=2 by default
ONLY=1 tests/test-progs/egdiff_attack/run_layer4.sh
```

## Residual caveats

- One run per value in the table above; `sweep_v1.sh` repeats each value `RUNS`
  times and reports a per-value match count and any `UNKNOWN` bytes, for a
  larger sample.
- The calibrated threshold assumes the hot/cold gap is large in the simulated
  cache hierarchy. It is (~14 vs ~120 cycles); on a hierarchy with a much
  smaller miss penalty the `t_arch * 4 >= t_cold` guard would discard more
  attempts rather than misreport.
- The harness still depends on the generated code keeping the helper load and
  `L_atk` adjacent (the distance-1 invariant). `run_layer5.sh` checks this
  topology with `objdump`; `run_layer4.sh` does not yet, so a compiler layout
  change could invalidate the Mode 1 measurement without an obvious failure.
  The `known=ff` gate would usually catch it, but an explicit topology check
  should be added.
- The drain uses `fence.i`; that is an eviction-ordering device for the
  experiment, not part of the mechanism.
