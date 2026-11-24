# Race, squash, and latency experiments

Status: experiments 1–3 complete. Mode 2's estimator is fixed by the
`flush_drain()` change: its off control is `known=00`, FLOP emits its own zero
(`known=ff`, `NO-MATCH`), and Layer 4 is 17/17.

## Experiment 3 — does a speculative fill survive a branch squash?

`tests/test-progs/egdiff_attack/branch_squash_probe.c` (run with `EGDIFF_ON=0`;
no LVP involved). The program trains a conditional branch, flushes the probe
array, absorbs the first post-flush access on a dummy line, then measures the
candidate line once.

| mode | dummy | **hot (0x42)** | cold (0x13) | line0 | sink |
|---|---|---|---|---|---|
| `arch` (bad path executed architecturally) | 116 | **16** | 146 | 114 | 0 |
| `squash` (bad path fetched, then branch mispredicted) | 116 | **16** | 114 | 114 | 170 |
| `none` (bad path routed to 0x7E; 0x42 untouched) | 117 | **116** | 114 | 114 | 8330 |

Interpretation:

- `none` is the null: the measured line is **cold** (116 cycles), even though
  the branch may speculate. The dummy absorbs the first-access artifact.
- `squash` leaves the line **hot** (16 cycles) while the architectural path is
  the good path (`sink = 0xAA = 170`, no bad-path value). The fill happened
  speculatively and survived a **branch-misprediction squash**.
- `arch` confirms the same hot value when the path executes architecturally.

Conclusion: speculative cache-fill survival is a **general cache property**, not
an artifact of reusing `IEW::squashDueToMemOrder` for value mispredictions.

Caveat: this is still gem5's cache model. It rules out the squash-path
objection, not the hardware-observation objection.

## Experiment 1 — minimum address delay for Mode 1

`poc_v1_specgvq_1shot.c` with `ATK_DIVS` compile-time variants. The prediction is
injected at dispatch; the division chain only postpones the mispredicting load's
data access/verification.

| divisions | result | known |
|---|---|---|
| 0 | NO-MATCH (partial) | e0 (3/8: bytes 5–7) |
| **1** | **MATCH** | **ff** |
| 2 | MATCH | ff |
| 3 | MATCH | ff |
| 4 | NO-MATCH at REPS=16 | ef (byte 4 unresolved; no fabricated byte) — see note |
| 6 | MATCH | ff |
| 8 | MATCH | ff |
| 10 | MATCH | ff |
| 12 | MATCH | ff |
| 0, off control | NO-MATCH | 00 |
| 12, off control | NO-MATCH | 00 |

Interpretation:

- The original zero-delay row was **invalid**: with no divisions, `x = ok|1`
  made the marker read `atk_buf[1] = 0`, not the sentinel, so the calibration
  line vanished and every attempt was `invalid`. After fixing the zero-delay
  address to `0`, it recovers **3/8** bytes (`0x33`, `0x22`, `0x11`), while
  **one** division recovers all 8. The race is therefore partial at zero delay
  and complete at one division; the 0 control is no longer a clean failure.
  Twelve divisions were conservative.
- The `DIVS=4` run at REPS=16 lost one byte (`known=ef`): byte 4 had **0/16
  informative attempts** (all `sentinel_only`), so the estimator abstained
  rather than fabricating. With `REPS=32`, the same configuration recovers
  **32/32 per byte** and MATCH. This is a measurement false negative at low
  attempt count, not a latency cutoff. Bytes 3 and 5 passed with only 5/16 and
  4/16 votes, so the REPS=16 estimator is marginal at this delay.
- Off controls at both ends recover nothing (`known=00`).

### Cycle margin (new trace)

Trace events: `schedule`/deferred `publish` → `load-sent` (probe cache request)
→ `MISMATCH` (verification) → `squash`. The probe is squashed before its
completion is delivered, so the **request** tick is the correct transient event;
the fill can land later.

| address delay | attempts with probe-send | pub→request avg | verify→request min / max / avg |
|---|---|---|---|
| 1 division | 125 / 128 | 12 cycles | **54 / 185 / 62 cycles** |
| 12 divisions | 127 / 128 | 93 cycles | **115 / 331 / 197 cycles** |

The 3–5 missing attempts are the sentinel-only/ambiguous cases the estimator
already abstains on. Even at the minimum delay, the probe's cache request is
~62 cycles ahead of the squash on average; at the PoC's 12 divisions it is
~197 cycles ahead. That is the measured race.  (Tick→cycle uses 500
ticks/cycle for this 2 GHz configuration; verify with `simTicks/numCycles`.)

## Experiment 2 — predictor latency

Implemented as `egdiffLatency` (normal) and `egdiffDeferredLatency` (after base
completion) on `LoadValuePredictionUnit`, published through an event in
`IEW::lvpSchedule`. Defaults are 0 to preserve the existing gates.

The first event-based attempt failed: normal +3 corrupted the run (`runnerup`
negative, abort at +5). The root cause was not the latency itself but the IQ:
`IEW::dispatchInsts` called `setLVPPrediction()` at dispatch, and
`InstructionQueue::addToProducers` marks a predicted destination ready
immediately. Consumers therefore issued before the delayed PRF write.

Fixed with a **pending-publish stage**:

- `DynInst` has a pending flag; the IQ marks a destination ready only when
  `lvpPredictionAvailable()`.
- `IEW` keeps a `seqNum`-indexed pending map serviced from `tick()` after squash;
  commit and squash cancel pending publishes.
- Deferred predictions carry a `pending` state in `EgDiffSpecEntry`;
  `publishDeferred()` makes the value visible at the publish tick. If the
  dependent completes first, its actual value is retained and the pending
  prediction is dropped.

Final matrix (`DIVS=12` unless noted):

| normal | deferred | result |
|---|---|---|
| 0 | 0 | MATCH, 16/16 per byte |
| 3 | 0 | **MATCH, 16/16** |
| 0 | 2 | **MATCH, 16/16** |
| 3 | 2 | **MATCH, 16/16** |
| 5 | 2 | **MATCH, 8/8** |

No negative `runnerup`, no aborts. Trace:

```
[tick:208594500] schedule-publish PC 0x10788 sn:94000 val 0 at 208596000 (+3)
[tick:208596000] publish PC 0x10788 sn:94000 val 0
[tick:200016000] schedule-publish PC 0x106d4 sn:92630 val 0 at 200017000 (+2 deferred)
[tick:200017000] publish PC 0x106d4 sn:92630 val 0 (deferred)
```

1500 ticks = 3 cycles at the 2 GHz tick rate. The probe-issue delay proxy
(0/1/3/5 `addi`) remains an independent sensitivity control and also recovers;
it is no longer a substitute for the predictor-latency model.
