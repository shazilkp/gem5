# SUBMISSION (revision 3) — Cross-Instruction Value Forwarding in a Global Load-Value Predictor

*Artifact-style write-up for simulated peer review. Revision 3 incorporates the
round-1 reviews in `REVIEWS_round1.md` and the independent eight-refutal audit
(`REVIEW_RESPONSE_independent.md`); round-1 dispositions are in
`REVIEW_RESPONSE.md`. Everything below is reproducible from this repository
except the two third-party inputs named in Appendix A.*

---

## Abstract

Global load-value predictors guess a load's value from the value of an
**earlier, different load** plus a learned offset. This paper re-implements one
such design, EgDiff (ISCA 2026), in gem5 and asks a security question the design
was not evaluated for: because the predictor's shared value queue is untagged
and its base operand is identified only by a *distance*, can one instruction's
value be forwarded into another instruction's register across a software
boundary?

We answer yes, with a functionally audited model, 117 automated conformance
checks, end-to-end demonstrations on crafted victims, a paper-faithful
speculative-queue recovery of one 8-byte Monocypher `crypto_verify32` key word,
and an NVQ-extension recovery of the zero-length SipHash-2-4 key. We include
same-binary negative controls, a behavioural smoke test on SPEC CPU2017, and a
measurement of how often the required code shape occurs in real functions. The attack needs **no mispredicted branch and no gadget in the
victim**, and **no predictor-table collision**. The distinguishing property is
the **untagged global value queue**: it removes both the table collision
(needed by Deng & Szefer) and the victim gadget (needed by FLOP). We are not
aware of a prior attack that forwards a victim's value into an attacker's
register through such a queue; the comparison is a taxonomy, not a reproduction
of those attacks.

**Scope statement.** This is a functional, design-level security analysis under
a source-level non-disclosure convention (a sandbox/JIT-like "the attacker's
code cannot name the secret", **not** an MMU boundary). It makes no
IPC/performance claim; the predictor's **+3 normal / +2 deferred latency** and
the cache-probe *timing* used to observe the leak **are** modelled by gem5. It
makes no privilege or cross-process claim. The
cache exfiltration is demonstrated on an undefended hierarchy: SafeSpec and
Delay-on-Miss are **not evaluated** and are expected to block the observation
while leaving the register-level forwarding intact.

---

## 1. Problem and claim

A local value predictor guesses load *L*'s value from *L*'s own history. A global
predictor instead computes

```
predicted(L) = value_of(the load d positions earlier) + learned_offset
```

with `1 <= d <= 32`. Two claims follow and are tested here:

- **A1 (mechanism).** If an attacker trains their own load to `distance = d`,
  `offset = 0`, and a victim's sensitive load lands exactly `d` back in the
  dynamic load stream, the predictor writes the sensitive value into the
  attacker's destination register.
- **A3 (opportunity).** Some real functions that load a sensitive value near
  their return satisfy this `d <= 32` precondition; long transform functions do
  not. A3 is a *precondition* result: where it holds we measured the distance,
  but the end-to-end attack is demonstrated only for `crypto_verify32` (last key
  word) and the zero-length SipHash path.

The novel element versus prior predictor attacks is the **untagged global
queue**: the attacker uses its own table entry, and the link to the victim is a
position in a queue that carries neither a base tag nor a base PC. This removes
the per-instruction collision required by Deng & Szefer and the victim-side
gadget required by FLOP. It does not remove the attacker's own value-to-address
probe.

## 2. Background: the EgDiff model

| Structure | Parameters (implemented) |
|---|---|
| Prediction table | 4K entries, 88 bits: 14-bit tag, 2-bit `u`, 3-bit FPC, 64-bit `diff`, 5-bit `distance` |
| Confidence | 3-bit forward probabilistic counter, vector `{1, ¼, ¼, ⅛, ⅛, ⅛, ⅛}`, saturates at 7 |
| Allocation | tag mismatch replaces only if `u == 0`, with probability 1/16; 10-bit TICK decays `u` |
| Global value queue | 64 entries = 32 speculative (lock-step with the LQ) + 32 committed (NVQ) |
| Deferred prediction | base not ready ⇒ store `distance`/`diff`, bind a one-to-one `Wake`; wake on base completion |
| Verification | predicted vs actual at LQ completion; mismatch ⇒ squash + suppress all predictions for 1024 instructions (paper §III-A) |
| Update | commit-stage distance polling (Algorithm 1); table written only at commit |

**Queue population (paper-confirmed).** Section III-B states: *“Regardless of
whether the prediction is valid or not, a new entry is allocated in the
speculative queue for all candidates.”* Table I shows entries with `VP = 0`
that are nevertheless present. The base is then retrieved *“using the distance
info”*; the entry carries `value`, `distance`, `diff` and flags, but **no base
PC or base tag**. Allocation is therefore not gated by EVES, and the victim's
key load occupies a slot. This is the exact property the attack uses. Quotes and
section references: `PAPER_NOTES.md`.

The optional **EVES-assisted population** (Section V-A) pushes *local
predicted* values into the queue at dispatch; it is not implemented. It is not
needed for the attack: the no-EVES baseline still fills the queue from
execution results (the paper's ablation: 3.11 % IPC / 22.45 % coverage without
EVES, versus 3.24 % / 26.15 % with it), and earlier base values can only help an
attacker. The omission is therefore conservative for the attack, not
exculpatory.

**Timing.** The predictor's **+3 normal** and **+2 deferred** latency are
modelled with a pending-publish stage (IQ ready gating, `IEW` pending map,
cancel on commit/squash). PRF port pressure and write elision are not modelled;
that is the remaining performance omission. Cache timing *is* modelled, because
the observable is a cache-probe hit/miss. The Mode 1 address-delay sweep recovers
**3/8** bytes at 0 divisions and **8/8** at 1; the 0-division control had to be
fixed to read the sentinel rather than `atk_buf[1]`. The cycle trace gives a
probe-request-to-verification margin of 54–185 cycles at 1 division (avg 62) and
115–331 at 12 (avg 197).

Other deviations: the PC hashes use `pc >> 1` (the illustrative `pc >> 2`
aliases two loads in one 4-byte word on RISC-V with compressed instructions);
`Wake` is stored as a sequence number, not a 5-bit index; the optional
EVES-assisted early population is not implemented (the core design allocates
every candidate and fills from execution).

## 3. Threat model

- **Single core, single thread, same address space.** The victim is an in-process
  component: a sandbox/JIT host helper, a library routine, a crypto check.
- The **attacker controls its own code and the call site**, including the loads it
  executes after the call returns.
- The **victim's value is not architecturally nameable by the attacker's code**.
  The target translation unit owns it as a `static` object; the attacker unit has
  no symbol reference, checked mechanically with `nm`. This is a same-process
  **non-disclosure convention**, comparable to a sandboxed script that cannot
  read engine-internal state. It is **not** an MMU-enforced isolation claim and
  it does **not** stop a native same-process attacker from computing the key
  address from the target's own code or from `/proc/self/mem`. The experiment
  instantiates the convention, not the sandbox.
- The attacker TU does reference `target_verify32_selfcheck` and
  `oracle_report_verify32`; the former reads the key inside the target TU and the
  latter is a post-hoc checker. The `nm` check covers only `secret_*`/`v32_key`
  symbols, so it proves non-naming, not non-reading. The measurement path itself
  uses only the public `target_verify32` entry point.
- **Observable channel:** the forwarded value drives a dependent load whose cache
  line survives the subsequent squash. This is a data-flow transient-execution
  channel. The *forwarding* is register-level and would survive SafeSpec/DoM; the
  *observation* is a speculative cache fill and would not. The paper reports the
  forwarding plus the observation on an undefended hierarchy; the fill surviving
  the squash is a gem5-modelled property (the value-misprediction squash reuses
  gem5's `squashDueToMemOrder`, whose MSHR/load handling determines survival),
  and real cores may differ.
- **Out of scope:** privilege escalation, cross-process leakage, IPC/performance,
  real hardware, timing channels beyond the cache probe, and sandbox
  instantiation.

## 4. Implementation and conformance

The model is `src/cpu/lvp/egdiff_predictor.{hh,cc}`, integrated through five O3CPU
hooks (dispatch, deferred wake, LQ verification, commit-stage update, ROB squash
rollback) plus configuration in `configs/example/egdiff.py`.

Conformance is enforced by six gates, each asserting the paper's semantics from
the trace/stats rather than "it runs":

| Gate | Focus | Checks |
|---|---|---|
| `run_layer1.sh` | commit-stage polling, stride latch, FPC/u saturation, TICK | 17 |
| `run_layer2.sh` | speculative queue, verification/squash, lastMisp, ROB rollback | 10 |
| `run_layer3.sh` | deferred `Wake` wakeup, early-completion ignore, normal path | 13 |
| `run_layer4.sh` | Mode 1 + Mode 2 end-to-end, controls, ownership split | 17 |
| `run_layer5.sh` | SipHash-2-4 real target, topology, controls | 24 |
| `run_layer6.sh` | Monocypher `crypto_verify32` real target, controls | 36 |
| **total** | | **117** |

The queue-allocation and base-selection rules the attack depends on are quoted
from the paper in `PAPER_NOTES.md` (Section III-B): every candidate allocates a
`SpecGVQ` entry, and the base is retrieved by distance with no base tag. The
gates test the implementation of those rules; they do not independently prove
the paper's text.

Two of the 117 checks are `sink`-equality sanity checks that are identical by
construction and cannot fail; the **effective evidence count is 115**. `sink` is
used only to detect gross divergence, not as an architectural checksum.

These are **trace-presence assertions, not an equivalence proof**. Several Layer-1
checks match a value anywhere in the trace rather than for the tested load in the
expected order; there is no differential co-simulation against
`experiment/egdiff_eval/egdiff_sim.py`; the deferred-binding-cleared-by-squash
stat is emitted but not asserted; and the 1/16 allocation rate and the exact
TICK period are checked for "some activity", not statistically. They establish
that the implemented model does what the plan says in the tested shapes, not
that it is the paper's model in every detail.

An independent subagent audit of Layer 3 (paper + plan only) found one blocking
harness defect (hash aliasing made a phase dead code) and one instrumentation
gap; both were fixed and the gate re-passed. The audit's report is retained.

## 5. Attack

**Mode 1 (in-flight or near-commit GVQ).** Train the attacker's load to `d`,
`diff = 0`; call the
victim once; execute the attacker's load immediately after the return with a
delayed address (register-only division chain). The predictor forwards the
victim's value; a dependent probe encodes it into the cache; verification
squashes, the line remains. The estimator uses per-attempt hot/cold calibration,
alternating architectural sentinels, a `known` mask, a **minimum of 2 votes** and
a **2× runner-up margin**.

**Mode 2 (commit-time capture).** The victim executes a serialising `fence.i`.
A capture body with exactly one constant load at `d = 1` makes Algorithm 1 poll
`32 → … → 2` and latch `diff = 0 − S = −S`; a later replay with a zero base
predicts `−S`. One capture per byte; the architectural value is routed to a probe
slot outside the measured range.

Mode 2's fence is a **harness ordering device**, not a security claim: gem5
models `FENCE.I` as a pipeline drain, but on RISC-V `FENCE.I` orders instruction
fetch, not data. The claim is that the victim load has committed before the
replay (guaranteed by the call/return and the harness fence) and the NVQ persists
by definition. Mode 2's estimator now includes a 3/7 vote floor, a 2× runner-up
margin, a `known` mask, byte-0 inclusion, and a `reg == SENTINEL` architectural
check. The decisive fix was a `flush_drain()` after `flush_l1()`: without it,
the eviction's outstanding MSHRs made a deterministic line win even with the
predictor off. With the drain, the `EGDIFF_ON=0` control is **`known=00`**. The
FLOP control legitimately emits its own constant-zero prediction (`known=ff`,
value 0) and is `NO-MATCH`; that is a real FLOP fill, not a measurement artifact.
Mode 2 remains a crafted-victim conformance experiment, not a real-target attack.

**Real targets.** SipHash-2-4 (`d = 5, 6`) **on the empty-message path only**:
with a non-empty message the key loads are followed by hundreds of state/message
loads, so `d > 32`. Monocypher `crypto_verify32` (`d = 7…14`) recovers only
`key[24..31]` (the last 8-byte word); the preceding word is at ≈15–22, the next
at ≈23–30, and the first at ≈31–38, so two more words are in-window in principle
but were not demonstrated. In both, the target TU owns the key and the attacker
TU never names it. The A4 harness can warm the key line with
`target_verify32_selfcheck()` and two `target_verify32(guess)` calls; in that
case the common path is base-available (NVQ), not the deferred path. A
`COLD_KEY=1` variant removes those calls, and a `VARY_GUESS=1` variant changes
the guess on every attempt. **Both cold and variable-input variants still
recover 8/8** in this model, and cold SipHash still recovers 16/16
(`RESULTS_target_controls.md`). A4 is therefore **paper-faithful SpecGVQ** with the NVQ-base extension off;
SipHash is an **NVQ-extension** result because strict SpecGVQ fails 0/16. The
warm-up is
disclosed but not required in these runs.

**Distance map.** `d` is a property of the compiled binary (compiler, flags,
library version), not of the source function. The SipHash constants must be
updated on codegen changes; A4's `d = 7…14` was measured in this build. Runtime
discovery of `d` (trying all 32 positions) is not demonstrated.

## 6. Measurement methodology

- **Coverage** = `egdiffCommittedPredicted / egdiffCommits`: the fraction of
  committing loads whose prediction verified.
- **Accuracy** = `egdiffCommittedPredicted / (egdiffCommittedPredicted +
  numPredictionIncorrect)`. The numerator is commit-based; the denominator also
  includes wrong-path loads that verified incorrectly. The two are not a strict
  partition (a wrong-path correct prediction is in neither), so "commit-based
  accuracy" is a shorthand, not a literal commit-only ratio. The `557.xz` pilot
  had `incorrect = 0` against 1.58M squashed predicted allocations, so the
  practical bias is small but unquantified.
- Dispatch-time ratios are reported as a diagnostic only. They are invalid as
  coverage on branchy code: `557.xz` dispatches 1.78M predicted loads against
  0.20M committed ones because of ~2M misprediction squashes.
- **Prevalence distance**: `n` = dynamic *load* instructions strictly between the
  last execution of a sensitive-load PC and the caller's marker load;
  `d = n + 1`; precondition `d <= 32`. Reported as the minimum over all sensitive
  PCs, last-execution semantics. The `PROT_NONE`/`SIGSEGV` probe marks every load
  touching the tagged page, an over-approximation of "reads the secret"; for the
  synthetic lookup/walk controls the winning PC can be a next pointer or a `used`
  flag, so their summary `d` is optimistic (the actual secret load is still
  `d <= 3`). `base_pc == d_pc` and `d_key == d` in all 6,300 rows, so those are
  consistency checks, not independent evidence.
- **Validation**: a known-load-count self test (currently `d = 1` only; an
  interior-distance case is still needed); the QEMU guest-PC field checked
  against `_start`; marker adjacency (zero loads between the callee `ret` and the
  marker in all 6,300 calls); and the `PROT_NONE`/`SIGSEGV` probe that validates
  the sensitive-PC set dynamically. `measure_d.py`'s "last 8 key loads" heuristic
  is tied to one disassembly; the offset-set assertion makes a codegen change
  fail the gate rather than silently shift the map.
- **Dispatch-order check**: the gem5 trace records every slot allocation and
  each prediction's `base_val`/`base_pc`, so the architectural distance can be
  compared with the order EgDiff actually indexes. The tool counts allocation
  events, not live queue entries, and is valid only in squash-free windows.
- **Modes**: 300 inputs per mode; `key`/`msg`/`len` replay the same 300-input
  sequence, so 600/900 calls are not independent samples and the 62.5% figures
  are mode blends.

## 7. Results

### 7.1 SPEC smoke test (first 30 M instructions from `_start`; not conformance)

| benchmark | committed loads | coverage | accuracy |
|---|---|---|---|
| 505.mcf | 6,426,344 | 64.48 % | 99.99 % |
| 544.nab | 5,527,140 | 27.48 % | 99.60 % |
| 548.exchange2 | 4,951,309 | 17.35 % | 99.52 % |
| 510.parest | 5,161,173 | 22.70 % | 99.83 % |
| 531.deepsjeng | 22,357 | 2.20 % | 95.16 % |
| **mean (5, unweighted)** | | **26.84 %** | **98.82 %** |

These are **not** the paper's 50 M warm-up + 100 M SimPoint windows. `505.mcf`'s
64.48 % is first-30 M startup behaviour (node/arc array initialisation); the
paper's Fig. 10(b) shows its steady-state coverage below 10 %. The four/five
benchmark means therefore must **not** be compared with the paper's 22–26 %
band, and the earlier "excluding mcf gives 22.9 %" reading is withdrawn. What
the table supports is narrower: on real RISC-V binaries the model predicts at
high accuracy with non-zero coverage, i.e. it behaves like a plausible value
predictor rather than a broken one.

We report all five attempted benchmarks; none is dropped to fit the paper. The
excluded set remains `519.lbm` (SE register-only spin; 4k loads in 10 M),
`503.bwaves` (exits after ~138k instructions), and `557.xz` (~2 M
misprediction squashes; 8× slower), with reasons. Configuration also differs
from the paper's Table II: TournamentBP instead of L-TAGE, 256/256 physical
registers instead of 168/168, no L3 or stride prefetcher, and the SPEC runs use
0-cycle predictor latency. F2 is a smoke test, not a conformance result.

### 7.2 Prevalence of the `d <= 32` shape (6,300 calls, 300 per input mode)

| target | kind | shape | d (mode) | stability | d ≤ 32 |
|---|---|---|---|---|---|
| `crypto_verify32` | real Monocypher | constant-time compare | 7 | 100 % at 7 | yes |
| `memcmp` | control implementation | early-exit compare | 1 | 100 % at 1 | yes |
| `strcmp` | control implementation | early-exit compare | 1 | 100 % at 1 | yes |
| hash lookup | control implementation | lookup | 2 | 62.5 % at 2 (1–2) | yes |
| list walk | control implementation | walk | 2 | 62.5 % at 2 (1–2) | yes |
| `crypto_chacha20` | real Monocypher | block cipher | 519 | 67 % at 519 | no |
| `blake2b` (keyed) | real Monocypher | block hash | 552 | 67 % at 552 | no |
| `aead_lock` | real Monocypher | composite | 1339 | 67 % at 1339 | no |

The four "control implementation" rows are hand-written targets in
`prevalence/targets/`, not the system libc; the real-libc `memcmp` probe is used
only for the dispatch-distance check. End-loading check/lookup shapes satisfy the
precondition; the hash/list exact distance is bimodal, so "precondition holds"
is not "attack demonstrated". Block primitives are structurally out: they load
the key once and then execute hundreds of loads of derived state. Variance is
classified per target (deterministic, attacker-selectable, or secret/external
state dependent).

### 7.3 End-to-end forwarding

| experiment | result | controls |
|---|---|---|
| Mode 1 (`SpecGVQ`, crafted victim) | 64-bit value recovered (5/5 secrets) | off/FLOP: NO-MATCH, 0 |
| Mode 2 (`NVQ` capture + replay, fenced victim) | 64-bit value recovered | off: `known=00`, `NO-MATCH`; FLOP: emits its own constant 0 (`known=ff`), `NO-MATCH` |
| SipHash-2-4, empty message | 16/16 key bytes, `d = 5, 6`; **NVQ extension** | off/FLOP: 0/16 correct |
| Monocypher `crypto_verify32` | 8/8 bytes of `key[24..31]`, `d = 7…14`; **strict SpecGVQ** (`EGDIFF_NVQ_BASE=0`) | off/FLOP: 0/8 |

In A4 the gem5 trace shows, for each key byte, 16 predictions whose `base_val` is
that byte (18 for `0x10`; the extra two are not filtered by dependent PC).
**With the NVQ-base extension disabled (`EGDIFF_NVQ_BASE=0`), A4 still recovers
8/8, `egdiffNVQBases=0`, and every normal prediction reports `base=specgvq` with
a nonzero `base_pc` naming the key-load PC (e.g. `0x10cce`).** This is the
paper-faithful speculative-queue result; the strict Layer-6 gate passes 36/36 at
12 divisions, and the 1- and 4-division variants also recover (2 divisions is a
measurement false negative). A4 with the default NVQ-base extension also
recovers, but the paper-faithful claim does not need it.

SipHash is different: with `EGDIFF_NVQ_BASE=0` it fails **0/16** at 1, 2, 4, and
12 divisions, and 0/16 with `COLD_KEY=1`. Its original 16/16 result is therefore
an **NVQ-extension** result, not a paper-faithful speculative-queue attack. The
crafted Mode 1 is SpecGVQ; Mode 2 uses the paper's NVQ update path. A one-call
`memcmp` probe gives
architectural `d = 1` and dispatch `d = 1` for both the equal and early-exit
cases; it has no branch between the last compared byte and the return, so it
does not exercise the wrong-path risk. A4's earliest byte is `d = 14` (margin
18), not the `d = 7` used for the prevalence margin argument. The current A4
gate has `numPredictionIncorrect = 355`; SipHash has 472 (earlier committed
notes reported 369/506; the counts are build-dependent). `known` is an
emission mask, not a provenance certificate: a byte is marked known when the
estimator accepts a cache line, and the oracle's exact-word comparison is the
correctness test. A stubbed-key run can emit zeros with `known=ff` and
`NO-MATCH`. Each leaked byte therefore costs a squash and a
1024-instruction global blackout: the attack has a loud per-leak signature, and
the "no timing claim" scope does not cover it.

The controls' `sink` equality in A4 is **not** an architectural checksum: it is a
harness accumulator that is identical by construction (see `RESULTS_A4.md`). The
architectural controls are `archpre self=OK repro=OK marker=OK` and the oracle's
`MATCH`.

Two further controls separate the observable from the recovery path
(`RESULTS_races.md`). An address-delay sweep shows the Mode 1 attack fails at
**0** register-only divisions and recovers all 8 bytes at **1**, so the race is
real and the 12-division PoC value is conservative. The cycle trace puts the
probe's cache request **54–185 cycles** before verification at 1 division
(avg 62) and **115–331 cycles** at 12 (avg 197); the modelled +3/+2 predictor
latency still recovers, as does a 3–5 cycle probe-issue proxy. A branch-squash control
(`EGDIFF_ON=0`) leaves the probe line at **16** cycles after a branch
misprediction versus **116** in the untouched-line null, so the fill survives a
branch squash as well as `squashDueToMemOrder`; survival is a general cache
property, not a path-specific artifact.

## 8. Related work

| work | predictor | collision needed | victim gadget | observable |
|---|---|---|---|---|
| Spectre | branch | — | yes | cache |
| FLOP (2025) | local LVP (M3) | **no** | **yes** | cache |
| SLAP (2025) | load address predictor | no | yes | cache |
| Deng & Szefer (2021) | local VP (LVP/VTAGE) | **yes** (index/tag) | yes | cache |
| ISER (2021) | recomputation | — | yes | cache |
| **this work** | **global VP (EgDiff)** | **no** | **no** | cache (transient fill) |

The novelty is the *global, untagged queue*: it supplies both properties at once.
Deng & Szefer need the attacker's table entry to collide with the victim's; FLOP
needs a victim-side gadget that turns the predicted value into an address. Here
the attacker's own entry is trained and the victim load is selected only by its
position in the queue; the attacker still supplies the value-to-address probe.
We have not reproduced either prior attack, so the table is a mechanism
comparison, not an experimental taxonomy.

SafeSpec / Delay-on-Miss / InvisiSpec hide speculative cache fills. They are
relevant to the *observation* and are **not yet evaluated here**; the
register-level forwarding would remain.

## 9. Limitations and threats to validity

1. **Predictor latency/IPC.** The paper's **normal +3** and **deferred +2**
   latencies are modelled through a pending-publish stage: a `DynInst` pending
   flag keeps the IQ destination not-ready, the publish is serviced from
   `IEW::tick`, and commit/squash cancel outstanding publishes. The matrix
   `0/0`, `3/0`, `0/2`, `3/2` and `5/2` all recover 8/8 with no corruption; the
   trace shows +3/+2. PRF port pressure and write elision remain unmodelled.
   A probe-issue delay proxy (3/5 instructions) is retained as an independent
   sensitivity and also recovers. Cache-probe timing *is* modelled. Mode 1's
   address-delay sweep recovers **3/8** bytes at 0 divisions and **8/8** at
   **1**; the cycle trace gives a probe-request-to-verification margin of
   **54–185 cycles** at 1 division (avg 62) and **115–331** at 12 (avg 197). A
   branch-squash control (`squash` line 16 cycles vs `none` 116) shows the fill
   survives a branch misprediction as well, so survival is a general cache
   property, not a `squashDueToMemOrder` artifact. The observation is still
   gem5-modelled, not a hardware result.
2. **Non-disclosure convention, not isolation.** The attacker cannot *name* the
   value, but a native same-process attacker could read it; no MMU, privilege or
   cross-process claim. The `nm` check covers only `secret_*`/`v32_key`, not
   `target_verify32_selfcheck`/`oracle_report_verify32`.
3. **Distance is not a margin.** The base is selected at an exact dispatch
   distance; one wrong-path load changes it. Architectural `d` is neither an
   upper nor a lower bound on dispatch `d` once slots are dropped at squash. A4
   is branchless and the `memcmp` probe has no branch in the window, so the
   measured agreement is not a positive control for divergence. For prevalence,
   in-window targets have `d <= 7` (25-load margin) and out-of-window targets
   have `d >= 519`; the large gap makes a flip implausible, but the argument is
   empirical, not a proof. `measure_dispatch_d.py` counts allocations, not live
   slots.
4. **Optional EVES-assisted early population not modelled.** The paper
   allocates every candidate (§III-B) and fills the queue from execution
   results; the optional EVES path pushes local predictions earlier. We
   implement the baseline. This cannot remove the attack — it can only make
   bases available sooner — though it could add attacker-visible bases from the
   local predictor, which we do not model.
5. **Mode 2 estimator and replay.** The estimator now has a 3/7 vote floor,
   2× margin, `known` mask, byte-0 inclusion and a `reg == SENTINEL` check.
   The original scan bias was caused by a missing `flush_drain()` after
   `flush_l1()`; with the drain, the `EGDIFF_ON=0` control is **`known=00`** and
   the on-run remains 8/8. FLOP emits its own constant-zero prediction
   (`known=ff`, value 0), which is a real fill and `NO-MATCH`. Mode 2 is still a
   crafted-victim conformance experiment, not a real-target attack, and its
   straight-line replay re-commits the sentinel and re-polls the entry, which is
   why it needs 16,800 victim calls rather than a branch-shadow replay. Its
   fence is a harness ordering device: gem5's `FENCE.I` drain is not a RISC-V
   data fence.
6. **Layer 5 is an NVQ-extension result.** Its estimator is weak (`REPS = 5`,
   fixed 20-cycle margin, bare majority, no vote floor), and strict SpecGVQ
   fails 0/16. It is useful as an extension control, not as a paper-faithful
   SpecGVQ demonstration.
7. **No single estimator across harnesses.** Mode 1/A4 use calibrated,
   vote-floor, 2×-margin, known-mask rules; Mode 2 and Layer 5 do not. §5's
   description is per-harness, not one design.
8. **Prevalence corpus is small and partly synthetic.** One real library, four
   hand-written controls, one function per library. The fault probe
   over-approximates; min-over-PCs is optimistic for lookup/walk; the three modes
   replay 300 inputs and are not independent samples.
9. **Narrow real targets.** A4 demonstrates one key word and is paper-faithful
   SpecGVQ when `EGDIFF_NVQ_BASE=0`; warm/cold variants with the NVQ extension
   also recover it. The full 32-byte key is not recovered. SipHash is
   empty-message only; its 16/16 recovery requires the NVQ extension (strict
   SpecGVQ fails 0/16). `memcmp`/`strcmp`/hash/list are precondition
   measurements, not attacks.
10. **Build sensitivity.** `d` depends on compiler, flags and library version.
    A3 is a property of these binaries, not of the source functions.
11. **Victim and attacker prediction behaviour is assumed.** The victim's
    sensitive loads are assumed predictable/pre-trained; a victim misprediction
    would squash the window. The attacker pays one squash and a 1024-instruction
    blackout per byte (current gates: 355 incorrect in A4, 472 in SipHash;
    build-dependent).
12. **Input variation.** The baseline victims use a constant key and a constant
    attacker input. A `VARY_GUESS=1` A4 variant (different guess every attempt)
    still recovers 8/8, because the shared `load64_le` PC alternates key/guess
    values and does not lock; that is a robustness result, not a full realistic
    workload. A MAC over a chosen message remains the missing external-validity
    experiment.
13. **Defences untested (deferred).** SafeSpec/DoM are expected to block the
    cache observation while leaving the forwarding intact. This is explicit
    future work, not part of the present scope; the impact claim therefore
    remains conditional on an undefended hierarchy.
14. **SPEC is a 30M window without warm-up**, unweighted, with three excluded
    benchmarks (`519.lbm`, `503.bwaves`, `557.xz`); `531.deepsjeng` is reported.
    Accuracy is not a strict commit-only ratio (§6). It is a startup smoke test,
    not a conformance proof; the 22.9 % band comparison is withdrawn.
15. **Reproducibility.** Monocypher is pinned (`third_party/README.md`) but not
    vendored; SPEC binaries are external, but `spec_f2.csv` is checked in. The
    working tree has an uncommitted `src/mem/cache/base.cc` hunk from the
    SafeSpec line; it only alters `isShadowNoAllocate` packets, which the EgDiff
    runs do not generate (shadow cache disabled), so it cannot affect the
    reported numbers, but the tree is not byte-identical to the reviewed commit.
16. **No co-simulation.** The 117 checks are trace-presence assertions; an
    equivalence run against `egdiff_sim.py` on randomised streams is missing.
17. **Mitigation not evaluated.** A 64-entry queue with a base tag (or base PC)
    is ~100 bytes of SRAM plus a comparator; tagging it would very likely
    eliminate the attack at modest cost. This is a design recommendation, not a
    measured fix.
18. **lastMisp is an approximation.** The window counts instructions dispatched
    after the squash, not only committed correct-path instructions; a later
    wrong-path instruction can still consume slots before its own squash. The
    Layer-2 gate checks the window by sequence numbers, not by this counter.
19. **Pipeline-detail scope.** Prefetch/zero-destination loads are now skipped
    from the GVQ/NVQ, but the model targets the paper's LQ=32 and silently drops
    allocations beyond 32, so LQ=48/64 configurations are not modelled. Predicted
    values are not sign/zero-extended for `lb`/`lh`/`lw`, because the access size
    is unknown at dispatch; the PoCs use `ld`/`lbu` with `diff = 0`, so their
    results are unaffected, but this is a fidelity gap for narrow signed loads.

## 10. Conclusion

EgDiff's untagged global value queue forwards one instruction's value to another
with no per-instruction collision and without requiring a gadget in the victim.
The design is demonstrated on crafted victims and on two narrow real paths (one
`crypto_verify32` key word and zero-length SipHash), the required code shape is
measured on real functions, and the model's behaviour is checked against SPEC.
The result is a design-level vulnerability in a *proposed* predictor, under a
source-level non-disclosure convention; the observable is a transient cache
channel that existing speculative-cache defences may already address, and the
defence experiment is the main open item.

---

## Appendix A — Artifact map and reproduction

```bash
# model + gates (gem5 RISC-V build)
scons build/RISCV/gem5.opt -j4
tests/test-progs/egdiff_attack/run_layer1.sh    # 17
tests/test-progs/egdiff_attack/run_layer2.sh    # 10
tests/test-progs/egdiff_attack/run_layer3.sh    # 13
tests/test-progs/egdiff_attack/run_layer4.sh    # 14
tests/test-progs/egdiff_attack/run_layer5.sh    # 24
tests/test-progs/egdiff_attack/run_layer6.sh    # 36

# behavioural ballpark (SPEC)
python3 experiment/egdiff_eval/spec_coverage/run_spec.py --maxinsts 30000000
# raw rows: experiment/egdiff_eval/spec_coverage/spec_f2.csv

# prevalence (host measurement, no gem5)
cd experiment/egdiff_eval/prevalence
# fetch the pinned Monocypher once:
#   git clone https://github.com/LoupVaillant/Monocypher.git third_party/Monocypher
#   git -C third_party/Monocypher checkout f8bb8dbd224556d58f5b4fa176c3f18fbfe50ebe
./tools/run_prevalence.sh

# dispatch-vs-architectural check (build the probe first; see RESULTS_dispatch_trace.md)
python3 tests/test-progs/egdiff_attack/measure_dispatch_d.py \
  /tmp/opencode/memcmp_probe --sensitive 0x1fdb4 \
  --qemu-log /tmp/opencode/q_equal.log --gem5-log /tmp/opencode/g5_equal/simout
```

Key files: `src/cpu/lvp/egdiff_predictor.*`, `configs/example/egdiff.py`,
`tests/test-progs/egdiff_attack/*` (PoCs, gates, `measure_d.py`,
`measure_dispatch_d.py`, `memcmp_probe.c`), `experiment/egdiff_eval/`
(`plan.md`, `IMPLEMENTATION.md`, `RESULTS_*.md`, `prevalence/`,
`spec_coverage/`, `REVIEWS_round1.md`, `REVIEW_RESPONSE.md`, `PAPER_NOTES.md`).
Paper PDF: `docs/papers/Revisiting_Global_Value_Prediction_A_Resurgent_Complement_to_Local_Predictors.pdf`.

Third-party pins: Monocypher `f8bb8dbd224556d58f5b4fa176c3f18fbfe50ebe`
(`prevalence/third_party/README.md`); SPEC CPU2017 RISC-V binaries are external.

## Appendix B — Claim-to-evidence map

| claim | evidence | artifact | limitation |
|---|---|---|---|
| F1 structural conformance | paper-confirmed allocation/base rule, audited model, 117 checks | layers 1–6; `PAPER_NOTES.md` | trace-presence, no co-sim; optional EVES early population absent; PRF port pressure not modelled (+3/+2 latency is) |
| F2 behavioural smoke test | 26.84 % 5-benchmark coverage, 98.82 % accuracy | `spec_coverage/` + `spec_f2.csv` | first 30 M from `_start`, no warm-up; **not** comparable to the paper's steady state |
| A1 mechanism | Mode 1/2, empty-message SipHash, verify32 word | layers 4–6 | same-process convention; Mode 2 is crafted-victim only |
| A2 specificity | off/FLOP controls | layers 4–6 | A4 `sink` is not a checksum; Mode 2 off is `known=00` |
| A3 opportunity | `d` table + margin | `prevalence/` | precondition only; controls synthetic; build-specific |
| A4 real-target forwarding | 8/8 bytes of `key[24..31]` at `d = 7…14`; strict `EGDIFF_NVQ_BASE=0` uses `specgvq`, zero NVQ bases | `run_layer6.sh`, `RESULTS_target_controls.md` | one word; branchless; SipHash is NVQ-extension only |
| F3 performance | — | — | **out of scope** |

## Appendix C — Round-1 review outcome

| reviewer | lens | score | recommendation |
|---|---|---|---|
| R1 | architecture | 6/10 | weak accept |
| R2 | security | 5/10 | borderline |
| R3 | methodology/artifact | 6/10 | borderline |

Consensus: the mechanism (A1) and the real-target demonstrations are credible and
well controlled; the threat model is a non-disclosure convention rather than a
sandbox, F2 is a ballpark rather than conformance, A3 is a precondition result,
the Mode 2/Layer 5 estimators are weaker than Mode 1/A4, and the defence
experiment is deferred (out of scope for this revision).

**Independent audit (R1–R8).** A second audit found eight refutals. Revisions
2–3 fix the load-bearing ones: commit polling uses the queue snapshot (R1);
`lastMisp` counts correctly-dispatched instructions (R2); the `+3/+2` latency is
modelled safely (R3); prefetch/zero-destination loads are skipped (R4); cold-key
and varying-guess controls are measured (R5/R6); Mode 2's `flush_drain` restores
a clean `known=00` off control (R7); and F2 is scoped as a startup smoke test,
with the 22.9 % three-benchmark comparison withdrawn (`505.mcf`'s 64.48 % is
startup) (R8). Remaining limitations are
subword extension, LQ>32, no co-simulation, no hardware, and the deferred
defence run. Full mapping: `REVIEW_RESPONSE_independent.md`.

Full round-1 reviews: `REVIEWS_round1.md`; dispositions: `REVIEW_RESPONSE.md`;
paper quotes: `PAPER_NOTES.md`.

## Reviewer packet (round 2)

Same three lenses, focused on the revised claims:

- **R1 (architecture):** does revision 2 correctly scope F1/F2? The paper
  confirms unconditional allocation and a distance-only base (Section III-B,
  `PAPER_NOTES.md`); does the optional EVES early-population omission matter for
  the security claim? Is the Mode 1 race now honestly an assumption?
- **R2 (security):** is the narrowed threat model (non-disclosure convention,
  conditional exfiltration) coherent? Is the FLOP/D&S comparison now correctly
  stated as a taxonomy? What is the minimum defence experiment for impact?
- **R3 (methodology):** are the corrected accuracy formula, prevalence caveats,
  estimator non-uniformity and reproducibility pins sufficient? Is the Mode 2
  control now reported honestly?
