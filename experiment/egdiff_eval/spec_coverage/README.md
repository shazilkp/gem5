# F2 — Behavioural conformance on SPEC CPU2017

Answers one question: **does the EgDiff model behave like the paper's predictor
on real programs?** Not "is it fast" — predictor latency and IPC are not
modelled. (The attack's cache-probe timing is a separate, modelled property; see
`SUBMISSION.md`.) This is the behavioural half of the fidelity claim (the
structural half is the audited implementation and the Layer 1–3 gates).

## Metric

Collected from `system.cpu.loadValuePredictor.*` in `stats.txt`:

All metrics are taken at **commit**, not at dispatch:

| quantity | definition |
|---|---|
| coverage | `egdiffCommittedPredicted / egdiffCommits` |
| accuracy | `egdiffCommittedPredicted / (egdiffCommittedPredicted + numPredictionIncorrect)` |

`egdiffCommittedPredicted` counts committing loads whose prediction verified.
Dispatch-time counts (`egdiffPredictions`, `egdiffDeferredPredictions`,
`egdiffDispatchedLoads`) are reported as a *diagnostic only*: on branchy code
they are inflated by wrong-path loads that are predicted and then squashed.
This is not hypothetical — `557.xz` (a mispredicted tight loop) showed 1.78M
dispatch predictions against 0.20M committed loads, i.e. a 99.8% "coverage"
that is pure speculation. The commit-based numbers are the architectural ones.

## Paper targets (for comparison only)

- accuracy > 99 %
- coverage ~22–26 %

We should land in the same ballpark, **not** match exactly: different ISA
(RISC-V vs AArch64), different hash function, shorter window, no SimPoints, and
`--caches --l2cache` rather than the paper's full hierarchy.

## Benchmarks

The default set is four single-command SPEC CPU2017 refrate benchmarks with
local inputs:

`505.mcf`, `544.nab`, `548.exchange2`, `510.parest`

`531.deepsjeng` is selectable (`--benches ...`) but is not in the default set;
see the exclusion reason below and the pilot number in the result section.

Also available but not in the default set:

- `519.lbm` — falls into a register-only loop under SE (4k loads in 10M
  instructions); its `reference.dat` input is also absent from the run dir.
- `503.bwaves` — exits after ~138k instructions, too short to train.
- `557.xz` — runs, but has ~2M branch-misprediction squashes in its first 1M
  instructions, making it ~8x slower per instruction. Useful as a stress case
  for the commit-vs-dispatch accounting; not representative.
- `531.deepsjeng` — spends its first 30M instructions zeroing a 720 MB hash
  table (stores, almost no loads), so a 30M window measures the allocator, not
  the engine. Needs a much larger window or the test input.

## Run

```bash
python3 experiment/egdiff_eval/spec_coverage/run_spec.py \
    --maxinsts 30000000 \
    --work /tmp/opencode/spec_f2
```

Outputs `spec_f2.csv` and a table. Each benchmark runs from its own SPEC
`run_base_refrate_riscv.0000` directory so relative input files resolve.

## Caveats to state with the numbers

1. **No warm-up split.** Stats are cumulative over the whole window, so they
   include predictor training (~1M instructions) and program start-up. The paper
   used 50M warm-up + 100M measured. Longer windows reduce the bias.
2. **Window length.** 30M instructions per benchmark by default; raise
   `--maxinsts` for a tighter estimate (cost is roughly linear: ~1.5 min per 30M
   on this machine).
3. **ISA/hash/config differ** from the paper, as above.
4. **Definitions.** Our coverage is the fraction of *committing* loads whose
   prediction verified. Accuracy is
   `committedPredicted / (committedPredicted + incorrectAtVerification)`: the
   numerator is commit-based, the denominator also counts wrong-path loads that
   verified incorrectly, so the two are not a strict partition. The observed
   effect is small (the `557.xz` pilot had `incorrect = 0` against 1.58M
   squashed predicted allocations), but it is not zero in principle. If the
   paper defines coverage differently, compare the appropriate pair rather than
   assuming they match.
5. **Single core**, matching the paper and the rest of this project.

## What the result is used for

If accuracy is in the high 90s and coverage is in the low-to-mid tens of
percent, the model behaves like the paper's design and the attack rests on a
realistic predictor. If coverage collapses, something in the model (hash,
allocation policy, confidence) is wrong and should be found before any further
attack claims.

## Result (30M instructions, commit-based)

| benchmark | committed loads | coverage | accuracy |
|---|---|---|---|
| 505.mcf | 6,426,344 | 64.51% | 99.99% |
| 544.nab | 5,527,140 | 28.38% | 99.60% |
| 548.exchange2 | 4,951,309 | 17.52% | 99.49% |
| 510.parest | 5,161,173 | 22.84% | 99.83% |

Mean over the four (unweighted over benchmarks, not pooled by load count):
**33.3% coverage, 99.73% accuracy**. Excluding `505.mcf` (pointer chasing, the
paper's own motivating shape, hence high coverage), the other three average
**22.9% coverage** (23.0% from unrounded values), at the lower edge of the
paper's reported 22–26%, with accuracy 99.5–99.8% against the paper's >99%.
The fifth attempted benchmark, `531.deepsjeng`, is excluded from both means: a
30M-instruction pilot measured **2.29% coverage / 95.35% accuracy** (22,357
committed loads) while the window was still zeroing a 720 MB hash table;
including it gives a five-benchmark unweighted mean of **27.11% / 98.85%**. The
exclusion is a workload-shape choice, not a filter on the attack.

Raw per-benchmark rows are checked in as `spec_f2.csv` (this directory).

Interpretation: the model behaves like the paper's predictor in the aggregate
(accuracy high, coverage in the expected band), which is what F2 is for. It is
not a reproduction of the paper's per-benchmark numbers: different ISA, hash,
memory hierarchy, window length and no SimPoints.
