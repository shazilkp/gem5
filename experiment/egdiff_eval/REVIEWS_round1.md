# Round-1 simulated reviews

Three independent reviewer agents read `SUBMISSION.md` (revision 1) plus the
supporting records and gate scripts. They did not modify the tree and did not
re-run the experiments. Their reviews are condensed here; the point-by-point
dispositions are in `REVIEW_RESPONSE.md`.

| reviewer | lens | score | recommendation |
|---|---|---|---|
| R1 | architecture | 6/10 | weak accept |
| R2 | security | 5/10 | borderline (weak reject for a security venue; weak accept for a workshop with narrowed scope) |
| R3 | methodology / artifact | 6/10 | borderline |

Mean 5.67/10. Editor's summary: **major revision** before the claims and the
artifact can be scored as a submission rather than a work-in-progress record.

---

## R1 — Architecture

**Summary.** The mechanism demonstration (A1/A2) is convincing and the
implementation record candid, but F1 (fidelity) and A3 (opportunity) rest on
assumptions the gates do not test. The load-bearing omission is the EVES
selector; the largest unmeasured risk is Mode 1's race.

**Strengths**

- End-to-end recovery on crafted victims and two real paths, same-binary
  controls, mechanical ownership split, `known` mask / `arch=OK` preconditions.
- The 17/10/13/14/24/36 = 114 check counts are honest.
- The retained audit trail (Layer-3 hash aliasing; invalid first Mode 2) is a
  good fidelity signal.
- The self-stride discovery (`d = 16`) is non-obvious and correct.
- `base_pc`/`base_val` trace instrumentation is the right evidence.

**Weaknesses (load-bearing)**

1. **EVES / queue population.** The attack needs every load, including the
   victim's, to allocate a `SpecGVQ` slot. The paper's selector may decide
   queue membership, not just which prediction is consumed. If so, the victim
   never enters the queue and the attack has no carrier. Quote the paper's
   allocation rule or run with the selector.
2. **Latency is not merely a performance omission for Mode 1.** The observable
   is a race between the probe (zero-latency forwarding) and the squash (address
   delayed by 12 divisions). The margin is assumed, not measured; a +3/+2
   sensitivity run is cheap.
3. **Commit-based accuracy as written is not self-consistent.** The text says a
   prediction counts only if its load commits, which would make accuracy ~100 %
   by construction. State the exact formulas.
4. **The 22.9 % comparison is a post-hoc three-benchmark mean.** With `mcf` the
   mean is 33.3 %, outside the band; `exchange2` is below it. Per-benchmark
   comparison and the paper's metric definition are missing.
5. **"Architectural distance is a lower bound" is not established** once slots
   are dropped at squash; the dispatch tool counts allocations, not live slots;
   the `memcmp` comparison has no branch in the window.
6. **The two real-target demos are degenerate along one axis each.** SipHash is
   `src_sz = 0` only; `verify32` yields one of four key words.
7. **Real-target overstatement.** "SipHash-2-4 (`d = 5, 6`)" without the
   empty-message qualifier is material.
8. **Base identity is partly circular for NVQ-sourced bases.** `base_pc = 0`
   there; the "16 each" count (18 for `0x10`) is not broken down by source.
9. **The gates are presence checks, not fidelity.** No co-simulation against
   `egdiff_sim.py`; several Layer-1 greps match anywhere; the 1/16 rate and
   TICK period are not statistically checked; squash-clears-wake is not
   asserted.
10. **Per-leak signature.** Every leaked byte is a value misprediction (369 in
    A4, 506 in SipHash) plus a 1024-instruction blackout; the "no timing claim"
    scope does not cover this.
11. **Mitigation missing.** A base tag/PC in the queue is cheap and would likely
    kill the attack.

**Direct answers.** Model parameters are internally consistent; gates are
adequate for the implemented shapes, medium confidence for fidelity. PRF
elision is performance-only; latency changes the Mode 1 race; EVES is
unresolved. Commit-based coverage is defensible; commit-based accuracy as
written is not. The dispatch argument is insufficient as stated; A4's base
trace is the best evidence and should be promoted.

**Recommendation.** Weak accept for the mechanism; do not claim F1/F2/A3 at
their current strength. If the paper's selector gates enqueueing, drop to
reject.

---

## R2 — Security

**Summary.** The mechanism is real and well isolated, and the artifact is strong
for a workshop. But the security claim rests on a source-level "cannot name"
convention that the code split does not enforce, and impact is conditional on
SafeSpec/DoM being absent. Novelty versus FLOP and Deng & Szefer is plausible
but not experimentally established.

**Strengths**

- A4 `base_val`/`base_pc` trace is the right evidence for "no table collision".
- `EGDIFF_ON=0` is a strong same-binary control; FLOP is a useful specificity
  control; `known` mask, 2× margin, calibration and `t_arch*4 >= t_cold` guard
  are designed against fabrication.
- Distance measurement is independently grounded (QEMU single-step + objdump;
  marker adjacency; dispatch trace for A4 and memcmp).
- Prevalence validation is ambitious: `PROT_NONE`/`SIGSEGV`, base-PC identity,
  last-execution, marker adjacency.
- Limitations 1–8 are candid.

**Weaknesses**

1. **The 25-load margin is not a stability argument.** The base is selected at
   an exact distance; one wrong-path load changes it. A4 is branchless; the
   early-exit controls are the risk.
2. **"Timing is not modelled" is false for the attack.** Recovery uses
   `rdcycle`; the intended claim is predictor latency/IPC.
3. **The A4 `sink` is not an architectural checksum.** It is a training/probe
   accumulator, identical by construction; the equality check proves nothing.
4. **"Absence of collision" is the wrong novelty sentence.** FLOP also has no
   collision; the distinguishing property is the untagged global queue, which
   removes both collision and victim gadget.
5. **The threat boundary is not enforced by the code split.** `nm` proves
   non-naming, not non-reading. A native attacker can compute the key address,
   scan `.rodata`, or use `/proc/self/mem`; the attacker TU also references
   `selfcheck`/`oracle`, which read the key. The "sandboxed script" analogy
   requires an actual sandbox.
6. **Novelty versus FLOP/D&S is asserted, not demonstrated.** The FLOP control
   is a different predictor, not a reproduction of FLOP's attack. State "we are
   not aware of a prior global-queue attack" and quote D&S's collision mechanism.
7. **Target set overgeneralized.** Only `verify32` (one word) and empty-message
   SipHash are attacked end-to-end; memcmp/strcmp/hash/list are precondition
   measurements; hash/list are bimodal; the full 32-byte key is not recovered.
8. **Distance map is build-specific.** A real attacker needs the build or
   runtime discovery (try all 32); not shown.
9. **"No gadget" applies to the victim only.** The attacker still needs the
   value-to-address probe.
10. **Victim predictions and `lastMisp` are not analyzed.** A victim key-load
    misprediction would squash the window; the attacker pays a blackout per byte.
11. **SafeSpec/DoM untested.** This is the impact experiment; at minimum make
    the abstract conditional.
12. **The surviving transient fill is a gem5 property**, not measured hardware.
13. **A4 warms the key line** with selfcheck + two verify calls, making the path
    base-available rather than deferred; disclose it.

**Recommendation.** Borderline. Weak reject for a venue requiring a realistic
sandbox or defence evaluation; weak accept for a workshop that accepts a
design-level mechanism with the scope narrowed.

---

## R3 — Methodology / Artifact

**Summary.** The metric arithmetic (`d = n+1`, loads only) is correct and
independently checkable; the A4 estimator is structurally sound. But the four
harnesses do not share one estimator — Mode 2 and Layer 5 are weaker and Mode 2's
negative control fabricates seven bytes — and reproducibility is overstated.

**Strengths**

- `d = n+1` verified against the CSV and `measure_d.py`; no off-by-one.
- Two independent tools agree on the `verify32` per-PC map.
- Marker adjacency (`loads_since_ret = 0`) is measured and true in all 6,300
  rows.
- A4 estimator: architectural sentinel outside measured lines, `arch_ok` on the
  re-executed return, `known == 0xff` required; a partial/fabricated byte cannot
  produce MATCH.
- Controls are same-binary for Layer 5/6.
- Audit history is a positive signal.

**Weaknesses**

1. **No single estimator; Mode 2's is not fabrication-free.** `argmax` with no
   vote floor; off control prints `3f3f3f3f3f3f3f00` (seven fabricated bytes);
   §7.3 reports only "0". `arch_ok` checks `atk_buf[0]`, not the replayed load;
   byte `0x00` is excluded.
2. **Layer 5's estimator is also weak:** `REPS=5`, fixed 20-cycle margin, bare
   majority, no vote floor/known mask; 0/16 in controls could be luck.
3. **"Commit-based accuracy" is a misnomer:** numerator commit-based,
   denominator also counts verification-time incorrect; not a partition. No gate
   validates the stat definitions.
4. **Prevalence prose is contradicted by its own CSV.** The winning base PC is
   not the same sensitive PC in every call for hash/list (225/375 and 375/225),
   and the controls have three instructions, not two, between `ret` and T2.
5. **Minimum-over-PCs is over-inclusive for the synthetic controls.** `listwalk`
   `d=1` is the next pointer; `hashlookup` `d=1` is the `used` byte. Actual
   secret load is `d<=3`, so the conclusion survives, but the summary is
   optimistic; `d_key == d` in all rows makes the refinement untested.
6. **Reproducibility overstated.** Monocypher is git-ignored; SPEC CSV is not
   checked in; Appendix commands consume `/tmp` artifacts no listed command
   produces.
7. **Post-hoc exclusions move both headline numbers toward the paper.** Dropping
   `mcf` takes 33.3→22.9; dropping `deepsjeng` (95.35% accuracy) takes 98.9→99.73.
8. **Build/toolchain sensitivity is not listed.** `verify32`'s `d=7` exists
   because of six epilogue restores; `-O`/ABI/inlining can move a target in or out.
9. **No uncertainty reporting.** 300 inputs × 3 modes are not 900 independent
   samples; 62.5% is a mode blend; SPEC is one run per benchmark.
10. **Self-test only exercises `d=1`;** an interior distance is needed to validate
    `n` vs `n+1`.
11. **`base_pc == d_pc` is not independent**; the same variable selects the
    minimum. Need a negative test.
12. **`measure_d.py`'s `[-8:]` heuristic is sound for this build but fragile**;
    the offset-set assertion prevents silent shifts.
13. **Means are unweighted** and should be labelled; pooled coverage is 35.2%.
14. **Layer 5 header says 22/22** while the table/script say 24.
15. **A4 says "16 predictions"** but the evidence file has 18 for `0x10`.
16. **A4 margin is 18, not 25:** earliest byte `d=14`.
17. **Working tree has an uncommitted `base.cc` hunk**; pin the commit or show it
    is inert.
18. **The "independent hand count" is not documented**; point to it or drop it.
19. **The fault probe over-approximates "reads the secret"**; state it.

**Recommendation.** Borderline; weak accept after reporting Mode 2's
`3f3f…` control, adding a vote floor/known mask, fixing the two prevalence
prose errors, committing/vendoring the third-party inputs, and stating
toolchain sensitivity.
