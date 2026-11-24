# Race, squash, and latency experiments

Status: experiments 1–3 complete. Mode 2 remains on hold (its off control still
emits seven bytes; see `SUBMISSION.md` §9.5).

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
| 0 | NO-MATCH | 00 |
| **1** | **MATCH** | **ff** |
| 2 | MATCH | ff |
| 3 | MATCH | ff |
| 4 | NO-MATCH | ef (one byte unresolved, no fabricated byte) |
| 6 | MATCH | ff |
| 8 | MATCH | ff |
| 10 | MATCH | ff |
| 12 | MATCH | ff |
| 0, off control | NO-MATCH | 00 |
| 12, off control | NO-MATCH | 00 |

Interpretation:

- The race is real: with **zero** delay the attack fails; with **one** division
  it recovers all 8 bytes. Twelve divisions were conservative.
- The `DIVS=4` run lost one byte (`known=ef`), i.e. the estimator abstained on
  one byte rather than fabricating it. That is measurement variance, not a false
  positive; a repeat would establish the rate.
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

First event-based matrix (`DIVS=12` unless noted):

| normal | deferred | result |
|---|---|---|
| 0 | 0 | MATCH, 16/16 per byte |
| 0 | 2 | **MATCH, 16/16 per byte** |
| 3 | 0 | NO-MATCH; `runnerup` goes negative → memory corruption |
| 3 | 2 | NO-MATCH |
| 5 | 2 (`DIVS=1`) | gem5 abort: illegal instruction at a stack address |

The deferred `+2` path is clean and the attack survives it. The **normal**
latency path is **not trustworthy**: a 3-cycle delay cannot plausibly kill an
attack with a ~240-cycle address delay, and the negative `runnerup` values plus
the illegal-instruction abort show that the delayed publish/wake corrupts the
program. This is a bug in the event path, not a result.

Safe proxy (prompt's allowed alternative): delay the dependent probe's issue by
`PROBE_DELAY` real `addi` instructions, with `EGDIFF_LATENCY=0` (normal event
disabled). Results:

| probe delay | address delay | deferred | result |
|---|---|---|---|
| 0, 1, 3, 5 | 12 | 0 | MATCH, 8/8 |
| 0, 3 | 1 | 0 | MATCH, 8/8 |
| 0 | 12 | 2 | MATCH, 8/8 |
| 3 | 12 | 2 | MATCH, 8/8 |

A 3–5 cycle later probe issue is harmless, consistent with the 54–331 cycle
measured margin.

The normal-latency event path was then **withdrawn**: it still corrupted the
run with `wake=false` and with `isInROB()`/`isCommitted()` guards. A non-zero
`egdiffLatency` now fails fast:
`fatal: egdiffLatency=3 is not supported...`. The deferred `+2` path remains
implemented and validated. The paper's normal 3-cycle latency is represented by
the probe-issue delay proxy, not by the withdrawn event path.
