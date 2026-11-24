# Response to round-1 reviews

Revision 2 of `SUBMISSION.md` addresses the round-1 reports in
`REVIEWS_round1.md`. Dispositions use:

- **fixed** — text or claim corrected in revision 2;
- **clarified** — claim kept but scope/qualifier added;
- **accepted** — agreed as a limitation; recorded, not solved;
- **open** — needs a new experiment or artifact;
- **reviewer error** — checked against the artifact and not sustained.

Round-1 scores: R1 6/10 weak accept, R2 5/10 borderline, R3 6/10 borderline.
Editor decision: **major revision**, then round 2.

## Cross-cutting changes in revision 2

1. The abstract and §9 now separate **predictor latency/IPC** (not modelled)
   from **cache-probe timing** (modelled).
2. The threat model is explicitly a **source-level non-disclosure convention**,
   not an MMU/sandbox claim; the `nm` check is described as proving non-naming,
   not non-reading.
3. The novelty sentence is now the **untagged global queue** (removes both the
   table collision and the victim gadget), and §8 calls the comparison a
   taxonomy, not a reproduction.
4. SipHash is qualified as **empty-message only**; `crypto_verify32` as **one
   8-byte key word**, with per-word reachability and the warmed key line
   disclosed.
5. The A4 **`sink` is no longer called an architectural checksum**; the real
   controls are `archpre` and the oracle. `RESULTS_A4.md` is corrected too.
6. Mode 2's **off control is reported** (`3f3f3f3f3f3f3f00`, seven wrong bytes),
   and its estimator weaknesses (no vote floor, no known mask, byte 0 excluded,
   weak `arch_ok`) are stated.
7. The **accuracy formula** is stated exactly and no longer described as a
   strict commit-only ratio.
8. The **EVES/queue-population question is resolved by the paper** (Section
   III-B): every candidate allocates a `SpecGVQ` entry, and the base is selected
   by distance with no tag. The optional EVES-assisted early population is the
   only model omission, and it can only make bases available sooner. Quotes:
   `PAPER_NOTES.md`.
9. **Prevalence prose** is corrected in `prevalence.md` (per-target base PCs;
   three instructions for controls) and the fault-probe over-approximation is
   stated.
10. **Reproducibility**: Monocypher is pinned in
    `prevalence/third_party/README.md`; `spec_f2.csv` is checked in; the
    `base.cc` hunk is explained as inert for EgDiff runs.

## R1 — architecture

| id | issue | disposition | where |
|---|---|---|---|
| R1-1 | EVES may gate queue allocation | **resolved by paper** (unconditional allocation, distance-only base) | §2, §9.4, `PAPER_NOTES.md` |
| R1-2 | 1024 window attribution; `fence.i`; squash reuse | fixed/clarified | §2 row, §5 Mode 2, §3 observable |
| R1-3 | Mode 1 latency race not measured | **measured** (delay sweep + cycle trace + +3/+2 model) | §7.3, §9.1, `RESULTS_races.md` |
| R1-4 | "architectural distance is a lower bound" | fixed | §9.3; A4 branchless, tool counts allocations |
| R1-5 | commit-based accuracy tautological | **reviewer error**, formula fixed | §6 gives numerator/denominator; `README.md` too |
| R1-6 | 22.9 % is post-hoc 3-benchmark mean | fixed | §7.1 reports all four, exclusions and numbers |
| R1-7 | SipHash/verify32 degenerate axes | fixed | §5, §7.3, §9.9 |
| R1-8 | `base_pc=0` / 18 count | fixed | §7.3 |
| R1-9 | gates are presence checks; no co-sim | accepted + open | §4, §9.16 |
| R1-10 | per-leak squash/blackout signature | fixed | §7.3, §9.11 |
| R1-11 | mitigation missing | fixed | §9.17 |

## R2 — security

| id | issue | disposition | where |
|---|---|---|---|
| R2-1 | 25-load margin is not base-identity stability | fixed | §9.3; exact distance, no positive divergence control |
| R2-2 | "timing is not modelled" false for the attack | fixed | Abstract, §2 "Timing" |
| R2-3 | A4 `sink` is not an architectural checksum | fixed | §7.3; `RESULTS_A4.md` |
| R2-4 | novelty sentence imprecise vs FLOP | fixed | Abstract, §1, §8 |
| R2-5 | threat boundary not enforced by code split | fixed/clarified | §3 |
| R2-6 | novelty vs FLOP/D&S asserted, not shown | clarified | §8 "taxonomy, not reproduction" |
| R2-7 | target set overgeneralized | fixed | §7.2 labels controls; §9.9 |
| R2-8 | distance map is build-specific | fixed | §5, §9.10 |
| R2-9 | "no gadget" means no victim gadget | fixed | §1 (attacker still supplies probe) |
| R2-10 | victim predictions / `lastMisp` | fixed | §9.11 |
| R2-11 | SafeSpec/DoM untested | accepted + **deferred** (out of scope) | Abstract, §9.13 |
| R2-12 | fill survival is a gem5 property | control added: survives a branch squash too; still gem5 | §3, `RESULTS_races.md` |
| R2-13 | A4 warms the key line | fixed | §5, §7.3 |

## R3 — methodology / artifact

| id | issue | disposition | where |
|---|---|---|---|
| R3-1 | no single estimator; Mode 2 fabricates 7 bytes | **fixed** by `flush_drain()`; off control `known=00`; Mode 2 still crafted-victim only | §5, §7.3, §9.5/9.7 |
| R3-2 | Layer 5 estimator weak | accepted | §9.6 |
| R3-3 | "commit-based accuracy" misnomer | fixed | §6 formula and non-partition caveat |
| R3-4 | prevalence prose contradicted by CSV | fixed | `prevalence.md` lines 79, 371 |
| R3-5 | min-over-PCs over-inclusive | fixed | §6, §9.8; `prevalence.md` caveats |
| R3-6 | reproducibility overstated | fixed | Appendix A, `third_party/README.md`, `spec_f2.csv`, §9.15 |
| R3-7 | post-hoc exclusions move numbers | fixed | §7.1 states both moves |
| R3-8 | toolchain sensitivity | fixed | §9.10 |
| R3-9 | no uncertainty reporting | fixed | §6 "Modes", §9.8 |
| R3-10 | self-test only `d=1` | accepted + open | §6 validation |
| R3-11 | last-8 heuristic fragile | clarified | §6 validation |
| R3-12 | `base_pc==d_pc` not independent | fixed | §6 |
| R3-13 | `d_key` never differs | fixed | §6 |
| R3-14 | Mode 2 `arch_ok` too weak | fixed | §5, §9.5 |
| R3-15 | Mode 2 excludes byte 0 | fixed | §5, §9.5 |
| R3-16 | alternating sentinels only Mode 1 | fixed | §5 (Mode 1 only) |
| R3-17 | means unweighted | fixed | §7.1 |
| R3-18 | paper metric caveat buried | fixed | §6/§7.1 |
| R3-19 | Layer 5 header 22/22 vs 24 | fixed | `IMPLEMENTATION.md` |
| R3-20 | 16 vs 18 predictions | fixed | §7.3 |
| R3-21 | A4 margin 18, not 25 | fixed | §7.3, §9.3 |
| R3-22 | uncommitted `base.cc` | fixed (explained inert) | §9.15 |
| R3-23 | "independent hand count" undocumented | fixed (claim removed) | §6 |
| R3-24 | fault probe over-approximates | fixed | §6, §9.8 |

## Open items, ranked

1. **SafeSpec/DoM defence run** (R2-11). Explicitly **deferred** by the authors:
   not part of this revision. The impact claim remains conditional on an
   undefended hierarchy.
2. **Variable-data victim** (R1-10, R2-10). Both real victims process constant
   data, so they never self-squash; a MAC over a chosen message would test the
   realistic case.
3. **Null-calibrated estimator for Mode 2** (R3-1). The vote floor, 2× margin,
   `known` mask, byte-0 inclusion and `reg == SENTINEL` check are in, but the
   the original scan bias was a missing `flush_drain()` after `flush_l1()`.
   With the drain, the off control is `known=00`; FLOP emits its own
   constant-zero prediction (`known=ff`, `NO-MATCH`). Mode 2 remains a
   crafted-victim conformance experiment, not a real-target attack.
4. **Layer 5 estimator** (R3-2). Add a vote floor and 2× margin; re-run and
   report emissions, not just correctness.
5. ~~**Mode 1 cycle-margin sensitivity** (R1-3)~~ — done:
   `RESULTS_races.md` (54–185 cycle margin at 1 division; 115–331 at 12).
   The normal +3 and deferred +2 latencies are now modelled via the
   pending-publish stage and both survive; the probe-delay proxy 3/5 is an
   additional sensitivity.
6. **Differential co-simulation** against `egdiff_sim.py` (R1-9).
7. ~~**Paper quote for EVES/queue allocation** (R1-1)~~ — resolved:
   Section III-B allocates all candidates; see `PAPER_NOTES.md`.
8. **Positive control for architectural-vs-dispatch divergence** (R1-4, R2-1):
   a branchy target near the boundary, live-slot replay.
9. **Second library and full `crypto_verify32` key** (R2-7, R1-7).
10. **Interior-distance self test** (R3-10).

## Reviewer error recorded

R1-5 claimed commit-based accuracy is ~100 % by construction because a
value-mispredicted load never commits. The checked-in formula is
`committedPredicted / (committedPredicted + numPredictionIncorrect)`, so the
squashed prediction *is* in the denominator. The reviewer's underlying point —
that the text was ambiguous and the numerator/denominator are not a partition —
is valid and is now fixed in §6.
