# SUBMISSION — Cross-Instruction Value Forwarding in a Global Load-Value Predictor

*Artifact-style write-up for simulated peer review. Everything below is
reproducible from this repository; the reviewer packet at the end defines the
three review roles.*

---

## Abstract

Global load-value predictors guess a load's value from the value of an
**earlier, different load** plus a learned offset. This paper re-implements one
such design, EgDiff (ISCA 2026), in gem5 and asks a security question the design
was not evaluated for: because the predictor's shared value queue is untagged
and its base operand is identified only by a *distance*, can one instruction's
value be forwarded into another instruction's register across a software
boundary?

We answer yes, with a functionally audited model, 114 automated conformance
checks, end-to-end demonstrations on crafted victims and on real library code
(Monocypher `crypto_verify32`, SipHash-2-4), clean same-binary negative controls,
a behavioural-conformance check on SPEC CPU2017, and a measurement of how often
the required code shape occurs in real functions. The attack needs **no
mispredicted branch and no clever gadget in the victim** and **no predictor-table
collision** — it is a property of the global value queue itself.

**Scope statement.** This is a functional, design-level security analysis. It
makes no performance claim (timing is not modelled), no privilege or
cross-process claim, and it does not evaluate cache-side-channel defences.

---

## 1. Problem and claim

A local value predictor guesses load *L*'s value from *L*'s own history. A global
predictor instead computes

```
predicted(L) = value_of(the load d positions earlier) + learned_offset
```

with `1 <= d <= 32`. Two claims follow and are tested here:

- **A1 (mechanism).** If an attacker trains their own load to `distance = d,
  offset = 0`, and a victim's sensitive load lands exactly `d` back in the
  dynamic load stream, the predictor writes the sensitive value into the
  attacker's destination register.
- **A3 (opportunity).** Real functions that load a sensitive value near their
  return satisfy this `d <= 32` precondition; long transform functions do not.

The novel element versus prior predictor attacks is the **absence of a
per-instruction collision**: the attacker uses its own table entry, and the link
to the victim is a position in an untagged queue.

## 2. Background: the EgDiff model

| Structure | Parameters (implemented) |
|---|---|
| Prediction table | 4K entries, 88 bits: 14-bit tag, 2-bit `u`, 3-bit FPC, 64-bit `diff`, 5-bit `distance` |
| Confidence | 3-bit forward probabilistic counter, vector `{1, ¼, ¼, ⅛, ⅛, ⅛, ⅛}`, saturates at 7 |
| Allocation | tag mismatch replaces only if `u == 0`, with probability 1/16; 10-bit TICK decays `u` |
| Global value queue | 64 entries = 32 speculative (lock-step with the LQ) + 32 committed (NVQ) |
| Deferred prediction | base not ready ⇒ store `distance`/`diff`, bind a one-to-one `Wake`; wake on base completion |
| Verification | predicted vs actual at LQ completion; mismatch ⇒ squash + suppress all predictions for 1024 instructions |
| Update | commit-stage distance polling (Algorithm 1); table written only at commit |

Deviations from the paper/plan, all deliberate and documented: the PC hashes use
`pc >> 1` (the illustrative `pc >> 2` aliases two loads in one 4-byte word on
RISC-V with compressed instructions); `Wake` is stored as a sequence number, not a
5-bit index; predictor latency (3 cycles normal, +2 deferred) and the
PRF-write elision are **not modelled**; the EVES hybrid is not implemented.

## 3. Threat model

- **Single core, single thread, same address space.** The victim is an in-process
  component: a sandbox/JIT host helper, a library routine, a crypto check.
- The **attacker controls its own code and the call site**, including the loads it
  executes after the call returns.
- The **victim's value is not architecturally nameable by the attacker's code**.
  The target translation unit owns it as a `static` object; the attacker unit has
  no symbol reference (checked mechanically). This is a same-process "cannot name
  it" boundary, comparable to a script that cannot read engine-internal state; it
  is **not** an MMU-enforced isolation claim.
- **Observable channel:** the forwarded value drives a dependent load whose cache
  line survives the subsequent squash. This is a data-flow transient-execution
  channel, so speculative-cache defences (SafeSpec, Delay-on-Miss) are relevant
  to the *exfiltration*, though not to the forwarding itself.
- **Out of scope:** privilege escalation, cross-process leakage, performance,
  real hardware, timing side channels beyond the cache probe.

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
| `run_layer4.sh` | Mode 1 + Mode 2 end-to-end, controls, ownership split | 14 |
| `run_layer5.sh` | SipHash-2-4 real target, topology, controls | 24 |
| `run_layer6.sh` | Monocypher `crypto_verify32` real target, controls | 36 |
| **total** | | **114** |

An independent subagent audit of Layer 3 (paper + plan only) found one blocking
harness defect (hash aliasing made a phase dead code) and one instrumentation
gap; both were fixed and the gate re-passed. The audit's report is retained.

## 5. Attack

**Mode 1 (in-flight).** Train the attacker's load to `d`, `diff = 0`; call the
victim once; execute the attacker's load immediately after the return with a
delayed address (register-only division chain). The predictor forwards the
victim's value; a dependent probe encodes it into the cache; verification
squashes, the line remains. The estimator uses per-attempt hot/cold calibration,
alternating architectural sentinels, a `known` mask, and a minimum vote margin.

**Mode 2 (commit-time capture).** The victim executes a serialising `fence.i`.
A capture body with exactly one constant load at `d = 1` makes Algorithm 1 poll
`32 → … → 2` and latch `diff = 0 − S = −S`; a later replay with a zero base
predicts `−S`. One capture per byte; the architectural value is routed to a probe
slot outside the measured range.

**Real targets.** SipHash-2-4 (`d = 5, 6`) and Monocypher `crypto_verify32`
(`d = 7…14`, generalised training). In both, the target TU owns the key and the
attacker TU never names it.

## 6. Measurement methodology

- **F2 coverage/accuracy are measured at commit**: a prediction counts only if its
  load commits. Dispatch-time ratios are invalid on branchy code — `557.xz`
  dispatches 1.78 M predicted loads against 0.20 M committed ones because of ~2 M
  misprediction squashes.
- **Prevalence distance**: `n` = dynamic *load* instructions strictly between the
  last execution of a sensitive-load PC and the caller's marker load;
  `d = n + 1`; precondition `d <= 32`. Reported as the minimum over all sensitive
  PCs, last-execution semantics, with base-PC identity checked.
- **Validation**: a known-load-count self test; the QEMU guest-PC field checked
  against `_start`; an independent hand count; marker adjacency (zero loads
  between the callee `ret` and the marker in all 6,300 calls); and a
  `PROT_NONE`/`SIGSEGV` probe that validates the sensitive-PC set dynamically.
- **Dispatch-order check**: the gem5 trace now records every slot allocation and
  each prediction's `base_val`/`base_pc`, so the architectural distance can be
  compared with the order EgDiff actually indexes.

## 7. Results

### 7.1 Behavioural conformance (SPEC CPU2017, 30 M instructions, commit-based)

| benchmark | committed loads | coverage | accuracy |
|---|---|---|---|
| 505.mcf | 6,426,344 | 64.51 % | 99.99 % |
| 544.nab | 5,527,140 | 28.38 % | 99.60 % |
| 548.exchange2 | 4,951,309 | 17.52 % | 99.49 % |
| 510.parest | 5,161,173 | 22.84 % | 99.83 % |
| **mean** | | **33.3 %** | **99.73 %** |

Excluding pointer-chasing `505.mcf`, the remaining three average **22.9 %
coverage**, inside the paper's reported 22–26 %, with >99 % accuracy. This is a
ballpark check that the model behaves like the paper's predictor, not a
reproduction (different ISA, hash, hierarchy, window; no SimPoints).

### 7.2 Prevalence of the `d <= 32` shape (6,300 calls, 300 per input mode)

| target | shape | d (mode) | stability | d ≤ 32 |
|---|---|---|---|---|
| `crypto_verify32` | constant-time compare | 7 | 100 % at 7 | yes |
| `memcmp` | early-exit compare | 1 | 100 % at 1 | yes |
| `strcmp` | early-exit compare | 1 | 100 % at 1 | yes |
| hash lookup | lookup | 2 | 62.5 % at 2 (1–2) | yes |
| list walk | walk | 2 | 62.5 % at 2 (1–2) | yes |
| `crypto_chacha20` | block cipher | 519 | 67 % at 519 | no |
| `blake2b` (keyed) | block hash | 552 | 67 % at 552 | no |
| `aead_lock` | composite | 1339 | 67 % at 1339 | no |

End-loading check/lookup shapes are eligible; block primitives are structurally
out because they load the key once and then execute hundreds of loads of derived
state. Variance is classified per target (deterministic, attacker-selectable, or
secret/external-state dependent).

### 7.3 End-to-end forwarding

| experiment | result | controls |
|---|---|---|
| Mode 1 (`SpecGVQ`, crafted victim) | 64-bit value recovered | off/FLOP: 0 |
| Mode 2 (`NVQ` capture + replay, fenced victim) | 64-bit value recovered | off/FLOP: 0 |
| SipHash-2-4 | 16/16 key bytes, `d = 5, 6` | off/FLOP: 0/16 |
| Monocypher `crypto_verify32` | 8/8 bytes of key[24..31], `d = 7…14`, 16 votes/lane | off/FLOP: 0/8 |

In A4 the gem5 trace shows, for each key byte, 16 predictions whose `base_val` is
that byte; when the base was in the queue, `base_pc` is the key-load PC
(`0x10f58 = lbu a5,0(a0)`, `0x10f7e = lbu a0,7(a0)`). A one-call `memcmp` probe
gives architectural `d = 1` and dispatch `d = 1` for both the equal and
early-exit cases.

## 8. Related work

| work | predictor | collision needed | victim gadget | observable |
|---|---|---|---|---|
| Spectre | branch | — | yes | cache |
| FLOP (2025) | local LVP (M3) | no | yes | cache |
| SLAP (2025) | load address predictor | no | yes | cache |
| Deng & Szefer (2021) | local VP (LVP/VTAGE) | **yes** (index/tag) | yes | cache |
| ISER (2021) | recomputation | — | yes | cache |
| **this work** | **global VP (EgDiff)** | **no** | **no** | cache (transient fill) |

SafeSpec / Delay-on-Miss / InvisiSpec hide speculative cache fills; they are
relevant to the observable channel and are **not yet evaluated here**.

## 9. Limitations and threats to validity

1. **No timing/IPC.** Predictor latency and PRF elision are not modelled; F3 is
   declared out of scope, so no performance claim is made.
2. **Same-process model, source-level boundary.** The attacker cannot *name* the
   value, but this is not MMU-enforced isolation; no privilege/cross-process claim.
3. **Architectural distance is a lower bound.** Wrong-path loads can only add
   loads, so `d` is optimistic. In-window prevalence targets have `d <= 7`
   (≥ 25 loads of margin); out-of-window targets cannot flip. A4 has no branch
   between the key load and the return; `memcmp` dispatch and architectural `d`
   agree. Branchier targets near the boundary are not covered.
4. **Prevalence corpus is small and one library.** Monocypher plus four
   hand-written controls; no second library.
5. **One real function per library.** SipHash and `crypto_verify32`; lane offsets
   in A4 are public code structure, only values come from the oracle.
6. **Defences untested.** SafeSpec/DoM are expected to block the *cache
   exfiltration* while leaving the forwarding intact; this must be measured, not
   assumed.
7. **SPEC coverage is a 30 M-instruction window without a warm-up split**, and
   four benchmarks were excluded with recorded reasons (`lbm` SE spin, `bwaves`
   too short, `xz` squash-heavy, `deepsjeng` hash-table zeroing).
8. **Measurement is a microbenchmark harness.** It demonstrates the mechanism;
   it is not an end-to-end exploit against a deployed sandbox.

## 10. Conclusion

EgDiff's untagged global value queue forwards one instruction's value to another
with no per-instruction collision and without requiring a gadget in the victim.
The design is demonstrated on crafted victims and on two real library routines,
the required code shape is measured on real functions, and the model's behaviour
is checked against SPEC. The result is a design-level vulnerability in a
*proposed* predictor, with the observable confined to a transient cache channel
that existing speculative-cache defences may already address.

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

# behavioural conformance (SPEC)
python3 experiment/egdiff_eval/spec_coverage/run_spec.py --maxinsts 30000000

# prevalence (host measurement, no gem5)
cd experiment/egdiff_eval/prevalence && ./tools/run_prevalence.sh

# dispatch-vs-architectural check
python3 tests/test-progs/egdiff_attack/measure_dispatch_d.py \
  /tmp/opencode/memcmp_probe --sensitive 0x1fdb4 \
  --qemu-log /tmp/opencode/q_equal.log --gem5-log /tmp/opencode/g5_equal/simout
```

Key files: `src/cpu/lvp/egdiff_predictor.*`, `configs/example/egdiff.py`,
`tests/test-progs/egdiff_attack/*` (PoCs, gates, `measure_d.py`,
`measure_dispatch_d.py`, `memcmp_probe.c`), `experiment/egdiff_eval/`
(`plan.md`, `IMPLEMENTATION.md`, `RESULTS_*.md`, `prevalence/`,
`spec_coverage/`).

## Appendix B — Claim-to-evidence map

| claim | evidence | artifact | limitation |
|---|---|---|---|
| F1 structural conformance | audited model, 114 checks | layers 1–6 | timing not modelled |
| F2 behavioural conformance | 22.9 % coverage, >99 % accuracy | `spec_coverage/` | short window, ISA differs |
| A1 mechanism | Mode 1/2, SipHash, verify32 | layers 4–6 | same-process model |
| A2 specificity | off/FLOP controls | layers 4–6 | ablation optional |
| A3 opportunity | `d` table + margin | `prevalence/` | architectural lower bound |
| A4 real-target forwarding | 8/8 key word at `d = 7…14` | `run_layer6.sh` | one function |
| F3 performance | — | — | **out of scope** |

---

## Reviewer packet — simulate three reviewers

Each reviewer should read this document plus, as needed, `IMPLEMENTATION.md`,
`RESULTS_*.md`, and the gate scripts. Each returns: summary, strengths,
weaknesses, detailed comments, and a recommendation (accept / weak accept /
borderline / weak reject / reject) with a one-line justification.

### R1 — Architecture reviewer

Focus: is the predictor model faithful, and does the security claim survive the
things the model does *not* include?

Questions to answer:
1. Are the table/queue/Algorithm-1 structures and parameters right, and is the
   conformance gate set adequate to establish that?
2. Does the absence of timing modelling (3-cycle, +2 deferred, PRF elision)
   undermine the security claim, or only the performance claim?
3. Is the "commit-based coverage" definition defensible, and is the comparison
   to the paper's 22–26 % honest?
4. Is the architectural-vs-dispatch argument (lower bound + margin + one measured
   comparison) sufficient, or is more needed?

### R2 — Security reviewer

Focus: is there a real vulnerability, is it EgDiff-specific, and what is the
realistic impact?

Questions to answer:
1. Is the threat model coherent (same-process "cannot name it" boundary)? Is the
   distinction from FLOP (no victim gadget) and Deng & Szefer (no table
   collision) correct and demonstrated?
2. Is the observable channel (transient cache fill) correctly characterised, and
   does the paper overstate impact by not testing SafeSpec/DoM?
3. Are the negative controls sufficient to exclude a measurement artefact?
4. Would a real attacker be able to satisfy the `d <= 32`, stable-distance
   precondition in the claimed target class?

### R3 — Methodology / artifact reviewer

Focus: are the numbers measured correctly and can they be reproduced?

Questions to answer:
1. Are the metric definitions and denominators right (loads only, `d = n+1`,
   commit-based coverage)?
2. Is the prevalence measurement sound (parser validation, sensitive-PC
   identification, last-execution, min over PCs, marker adjacency)?
3. Are the controls same-binary and meaningful, and is the estimator free of
   fabrication (`known` mask, vote margins)?
4. Is the artifact reproducible from the commands and files listed, and are the
   limitations stated honestly?

### Rebuttal cues (available evidence, not arguments)

- F1: `run_layer1..3.sh`; the Layer-3 audit report.
- F2: `spec_coverage/README.md` and CSV; the `xz` dispatch/commit divergence.
- A1: `run_layer4..6.sh`; ownership checks (`nm`) and per-PC trace attribution.
- A3: `prevalence/results/prevalence.md`, including the validation section.
- A4: `RESULTS_A4.md`; `RESULTS_dispatch_trace.md` for base identity and the
  architectural-vs-dispatch comparison.
- Open: defence test (SafeSpec/DoM), second library, ablation of order/deferred.
