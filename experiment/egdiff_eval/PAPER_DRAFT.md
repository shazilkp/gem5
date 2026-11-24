# Distance Is Not Identity: Cross-Instruction Value Forwarding in EgDiff's Untagged Global Value Queue

**A design-level vulnerability analysis in gem5**

*Anonymous workshop submission — author names and affiliations omitted for review.*

---

## Abstract

Global load-value predictors guess a load's value from the value of an **earlier, different load** plus a learned offset. EgDiff (ISCA 2026) makes this practical with a compact table, a speculative global value queue, commit-stage training, and a distance-polling update rule. The design is evaluated for accuracy, storage, and IPC, but not for security. This paper asks a narrow question: because EgDiff allocates a queue entry for **every** dispatched load and selects the base operand solely by **dynamic distance**, with no base tag or producer identity, can one instruction's value be forwarded into another instruction's register across a software boundary?

We answer yes, in a gem5 model, under a same-process **non-disclosure convention**: the victim owns a value the attacker's code cannot name, and the attacker's load consumes whatever load sits at distance *d*. The attacker needs no mispredicted branch in the victim, no victim-side value-to-address gadget, and no predictor-table collision. In the model, a crafted 64-bit value is recovered through the speculative queue; the last 8-byte word of a Monocypher `crypto_verify32` key is recovered byte for byte (*d* = 7–14) through the **speculative queue** with the NVQ-base extension disabled; and the 16-byte SipHash-2-4 key is recovered on the empty-message path (*d* = 5–6) through the **NVQ-base extension**, not the paper's speculative prediction path. Same-binary controls with the predictor disabled and with a local constant-value predictor recover none of the secret. The required code shape occurs in end-loading comparisons and lookups (*d* = 1–7) and is absent from the block primitives we measured (*d* ≥ 85).

This is a **design-level vulnerability analysis**, not a hardware exploit. We do not demonstrate a privilege or cross-process boundary, a full 32-byte key, a non-empty SipHash path, a defence, or a hardware result. The attacker still needs offline knowledge of *d* and its own value-to-address probe. We conclude that the untagged base is the security-relevant design choice, and that tagging the base or restricting cross-PC forwarding would remove the primitive at a modest but unmeasured cost.

**Keywords:** value prediction, global value prediction, microarchitectural security, transient execution, gem5, EgDiff.

---

## 1. Introduction

Value prediction (VP) breaks true data dependencies by guessing an instruction's result before it executes. A local predictor uses the history of one static instruction; a global predictor uses values produced by other dynamic instructions. Local predictors dominate both the performance and security literature; global prediction has received little attention since gDiff in 2003. EgDiff (Yang et al., ISCA 2026) revisits it and reports a 4.37 % average IPC speedup with more than 99 % accuracy, a 44 KB table, and a 19 KB hybrid with a local predictor that reaches 6.16 % [EgDiff]. The design uses one stride plus a distance field, commit-stage training, a probabilistic confidence counter, and a speculative value queue with deferred prediction.

The EgDiff paper evaluates performance, accuracy, and storage. It does not evaluate security; a text search for *security*, *attack*, *Spectre*, *side channel*, *confidential*, and *transient* returns no security analysis. The design does contain a structural choice that deserves scrutiny. Section III-B states:

> "Regardless of whether the prediction is valid or not, a new entry is allocated in the speculative queue for all candidates." — EgDiff, Section III-B (p. 579)

and:

> "The speculative queue is then accessed using the distance info to retrieve the base value. The predicted value is computed as the sum of the base value and the stride value." — EgDiff, Section III-B (p. 579)

The queue entry in Table I has a value, a distance, a diff, and the flags `VP`, `Misp`, `Avail`, and `Wake`. It has **no base PC, no base tag, and no producer identity** (p. 580). The speculative segment is "indexed identically to the LQ, as both are circular buffers allocated and squashed in lockstep" (Section III, p. 578). A load's queue position is its position in the dynamic load stream.

The security question follows. If the queue is a shared, position-indexed structure holding the values of all loads, and the base of a prediction is selected only by distance, then a load can consume the value of *any* load that happens to sit *d* positions earlier. It need not be the same static instruction, and it need not write a register the consumer reads. That is a cross-instruction forwarding primitive: the identity of the producer is replaced by a number.

We make five contributions.

1. **We identify the untagged, distance-selected base as the security-relevant property of EgDiff.** Prior value-predictor attacks rely on a table collision between attacker and victim, or on a victim-side value-to-address gadget. EgDiff's global queue removes both: the attacker trains its own table entry, and the victim's value is selected by queue position. The attacker still needs its own probe, but not a gadget in the victim.

2. **We implement EgDiff in gem5 and demonstrate the primitive end to end.** In the model, a crafted 64-bit value is forwarded into an attacker register; a Monocypher `crypto_verify32` key word is recovered; and a SipHash-2-4 key is recovered on the empty-message path.

3. **We separate the mechanism from the observation.** The forwarding is register-level; the observation is a speculative cache fill that survives the squash in gem5. We test the observation with a branch-squash control, an address-delay sweep, and a latency matrix.

4. **We measure the precondition on real binaries.** Over 6,300 calls in eight functions, end-loading comparisons and lookups satisfy *d* ≤ 32; block ciphers and keyed hashes do not (*d* ≥ 85).

5. **We state the boundary of the claim.** The result is a design-level vulnerability analysis under a source-level non-disclosure convention, not a real-world exploit, a privilege boundary, a hardware result, or a defence evaluation. We propose a fix but do not measure it.

### What this paper does not claim

- **No real-world exploit.** The attacker and victim share an address space. The victim's value is not architecturally *nameable* by the attacker's code, but a native same-process attacker could read it directly. Section 3 explains why this is a non-disclosure convention, not an MMU or sandbox boundary.
- **No hardware vulnerability.** All attack measurements are in gem5; the cache-fill survival that makes the observation visible is a model property.
- **No privilege or cross-process leak, and no performance claim.** Single core, single thread, same address space. We do not reproduce the paper's 4.37 % IPC or 99 % accuracy; the SPEC numbers are a smoke test.
- **No full key recovery.** One 8-byte word of a 32-byte `crypto_verify32` key; a 16-byte SipHash key only on the empty-message path.
- **No defence evaluation and no conformance proof from the 117 assertions.** SafeSpec, Delay-on-Miss, InvisiSpec, and ISER are not evaluated; the assertions are mostly trace-presence checks.
- **No reproduction of FLOP or Deng & Szefer.** Our FLOP control is the local predictor already in the repository, not FLOP's attack; the comparison is a mechanism taxonomy.

The appendices classify the artifacts, give exact commands, and state what a skeptical reader should not infer.

---

## 2. Background

### 2.1 From local to global prediction

Value prediction guesses an instruction's result and lets dependents execute before the result is known. The predictor is verified when the instruction executes; a mismatch squashes the dependent window. Local predictors index by PC and learn the value or stride of one static instruction. Global predictors use values produced by other dynamic instructions. gDiff (Zhou, Flanagan, and Conte, 2003) keeps a global value queue of recent results, stores several candidate strides per PC-indexed entry, and selects the base by a distance index [gDiff]. EgDiff is a modern redesign of gDiff [EgDiff].

### 2.2 EgDiff's structures

EgDiff's table has 4,096 entries and a logical 88-bit entry: a 14-bit PC tag, a 2-bit usefulness counter `u`, a 3-bit forward probabilistic counter (FPC), a 64-bit `diff`, and a 5-bit `distance` (Section III-A; `EgDiffTableEntry` in `src/cpu/lvp/egdiff_predictor.hh:44-52`). The FPC uses the probability vector {1, ¼, ¼, ⅛, ⅛, ⅛, ⅛} and saturates at 7. A new entry is allocated on a tag mismatch only if `u == 0`, with probability 1/16; a 10-bit TICK counter decays `u`. A global `lastMisp` mechanism suppresses predictions for 1,024 instructions after a value misprediction (Section III-A; `src/cpu/lvp/egdiff_predictor.hh:116-120,200-203`).

The global value queue (GVQ) has 64 entries: 32 speculative entries that mirror the load queue (LQ), and 32 non-speculative entries (NVQ) that hold committed values (Section III; `src/cpu/lvp/egdiff_predictor.cc:220-222`). The speculative segment is allocated and squashed in lockstep with the LQ; on a squash, entries younger than the squash point are invalidated, while the NVQ and table remain (Section III; `src/cpu/lvp/egdiff_predictor.cc:325-350`). At commit, the value is pushed into the NVQ and used to train the table (Section III-B; `src/cpu/lvp/egdiff_predictor.cc:28-124`).

### 2.3 The queue and Algorithm 1

EgDiff predicts at dispatch. The PC indexes the table; if the tag matches and the FPC is 7, the stored `distance` and `diff` are used. The base comes from the speculative queue at index `curIdx - d`, where `curIdx` is the new load's slot index before it is pushed. The model can also read the NVQ at `nvq[d - curIdx - 1]` when the base has committed; that is the **Rule C extension**, not the paper's prediction text, and is disabled for strict-fidelity runs. The predicted value is `base + diff` (Section III-B; `src/cpu/lvp/egdiff_predictor.cc:144-210`). If the base is in flight with no value, EgDiff binds a one-to-one `Wake` field and publishes when the base completes (Section III-C; `src/cpu/lvp/egdiff_predictor.cc:260-301`; `src/cpu/o3/iew.cc:665-676`).

Algorithm 1 runs at commit. It computes `delta_a = actual - NVQ[d]`, compares it with the predicted `diff`, and on a mismatch resets the FPC and `u` and decrements the distance, recomputing the stride at `d-1`; on a match it probabilistically increments the FPC and increments `u` (Algorithm 1, p. 579; `src/cpu/lvp/egdiff_predictor.cc:57-115`). The paper calls this *distance polling*: it replaces gDiff's multiple candidate strides with one stride and a distance that walks toward the most recent stable correlation.

### 2.4 Deferred prediction and latency

EgDiff has two prediction modes. The paper states: "EgDiff includes two prediction modes: a normal prediction with 3-cycle latency, and a deferred prediction with 2 additional cycles. The first cycle reads the Wake field from the GVQ entry to identify the bound entry. The second cycle accesses this dependent entry to retrieve the diff and computes the predicted value" (Section IV, p. 581). Our model implements a pending-publish stage: the destination register is not-ready until the publish tick, and commit and squash cancel outstanding publishes (`src/cpu/o3/iew.cc:505-660`; `configs/example/egdiff.py:33-34`). The latency parameters default to 0; the gated attack runs use the default. Section 6.6 reports a separate matrix that turns the paper's 3/2 latency on.

### 2.5 The design property the attack uses

Two sentences from the paper are load-bearing. First, allocation is unconditional:

> "Regardless of whether the prediction is valid or not, a new entry is allocated in the speculative queue for all candidates." — Section III-B (p. 579)

Second, the base is selected by distance alone:

> "The speculative queue is then accessed using the distance info to retrieve the base value." — Section III-B (p. 579)

Table I's fields are `Index`, `Value`, `Distance`, `Diff`, `VP`, `Misp`, `Avail`, and `Wake` (p. 580). There is no base PC, no base tag, and no producer identity. The speculative segment is "indexed identically to the LQ" (Section III, p. 578). The non-speculative segment stores values only; our NVQ is `std::array<uint64_t, 32>` (`src/cpu/lvp/egdiff_predictor.hh:221`), so it cannot carry a producer identity even in software.

The paper's optional EVES-assisted population pushes local predictions into the queue at dispatch (Section V-A). It does not gate allocation; it only makes some base values available earlier. Our model implements the no-EVES baseline. The paper's ablation reports 3.11 % IPC and 22.45 % coverage without EVES versus 3.24 % and 26.15 % with it (Section V-A), so the no-EVES configuration is a valid EgDiff. Earlier base values can only help an attacker; the omission is conservative for the attack, not exculpatory.

---

## 3. Threat model

We assume a single core, a single thread, and a single address space. The victim is an in-process component: a library routine, a sandbox or JIT host helper, or a cryptographic check. The attacker controls its own code and the call site, including the loads it executes after the call returns. The victim's sensitive value is loaded by the victim, used in registers, and not returned to the caller. The attacker's measurement path has no architectural reference to the value.

The non-disclosure convention is instantiated in the artifact. The target value and the SipHash key are `static` objects in a separate translation unit (`tests/test-progs/egdiff_attack/egdiff_secret.c:31,80-83`; `target_verify32.c:40-46`). The attacker translation unit has no undefined reference to a `secret_*` or `v32_key` symbol, and the linked binary has no global symbol with those names (`run_layer6.sh:52-69`; `run_layer4.sh:80-93`). The measurement path calls only the public helper (`victim_value`, `victim_tag`, `target_verify32`). The post-hoc oracle reads the value to check the answer and runs after the measurement.

This convention is **not** an MMU or sandbox boundary, and we do not present it as one. The `nm` check proves **non-naming**, not non-reading. A native same-process attacker could compute the address from the target's code, scan `.rodata`, or use `/proc/self/mem`; none of these is prevented by the artifact. The attacker translation unit also references `target_verify32_selfcheck` and `oracle_report_verify32`, which read the key, so the symbol check covers only the key symbols. The experiment instantiates the convention, not the boundary.

The observable channel is a cache probe. The forwarded value drives a dependent load whose cache line survives the subsequent squash. This is a data-flow transient-execution channel. The **forwarding** is register-level and would survive a defence that only filters speculative cache fills; the **observation** is a speculative cache fill and would not. The fill's survival is a gem5-modelled property: the value-misprediction squash reuses gem5's `squashDueToMemOrder`, whose MSHR and load handling determine survival (`src/cpu/o3/lsq_unit.cc:1309-1318`). A branch-squash control (Section 6.6) shows the same survival after a branch misprediction, so it is a general property of this cache model. Real cores may differ. Out of scope: privilege escalation, cross-process leakage, IPC, real hardware, timing channels beyond the cache probe, sandbox instantiation, and defence evaluation.

---

## 4. Design and mechanism

### 4.1 Training the attacker's entry

The attacker chooses one static load, `L_a`, and uses the same PC for training and measurement. Training runs a loop in which a *carrier* load produces a value `v_i`, then `d-1` noise loads produce independently varying values, then a store writes `v_i` to the marker buffer, and then `L_a` loads the marker buffer (`poc_verify32.c:198-214`; `poc_v1_specgvq_1shot.c:205-210`). The marker is exactly `d` dynamic loads after the carrier, and both load `v_i`; every other distance has a difference that varies with `i`. Algorithm 1's polling therefore walks from 32 down to `d` and latches `diff = 0` there. The table entry is tagged by `L_a`'s PC and reaches `ctr = 7` after the FPC ramp. The attacker never trains, names, or collides with the victim's table entry.

The training body is structured so that no other distance is a fixed point. In Mode 1, the carrier and marker are adjacent (`d = 1`) and the pairs are `(p, p)`; a self-distance cannot lock because the pair values vary. In A4, the carrier is followed by `d-1` varying noise loads, and the store to the marker buffer does not advance the load distance (`poc_verify32.c:198-214`). The artifact records a failure mode in which a linear value sequence locks at a self-distance (the "self-stride trap") and avoids it (`IMPLEMENTATION.md:482-491`). This is an engineering detail, not a security assumption: the attacker controls its own value sequence.

### 4.2 The victim's value becomes the base

The victim executes a load `L_v` of the secret. Because every candidate allocates a speculative slot, `L_v` has a slot even if it was not predicted, even if the predictor abstained, and even if the table entry indexed by `L_v`'s PC belongs to another static instruction (Section III-B; `src/cpu/lvp/egdiff_predictor.cc:212`). When `L_v` completes, its value is written into that slot; when it commits, the value is pushed into the NVQ.

The attacker then executes `L_a` so that it is the `d`-th load after `L_v`. At `L_a`'s dispatch, the table entry matches `L_a`'s PC, the FPC is 7, and the stored distance is `d`. The base lookup is `specGVQ[curIdx - d]`; that slot is `L_v`'s slot, because the queue is position-indexed and untagged. If the base has a value (speculative completion, deferred publication, or NVQ), the predictor computes `value(L_v) + 0` and writes it into `L_a`'s destination physical register. It also wakes `L_a`'s dependents. If the base is still in flight with no value, EgDiff binds a `Wake` and publishes when the base completes.

Nothing in this path checks that `L_v` and `L_a` are the same static instruction, that `L_v` writes a register `L_a` reads, or that `L_v` belongs to the same software component. The only link is the number `d`. That is the sense in which distance is not identity.

### 4.3 Squash and observation

The attacker's dependent probe is a load whose address is derived from the forwarded value, for example `probe[(value & 0xff) * 64]`. It issues as soon as the predicted value is available, before `L_a`'s data access. `L_a`'s address is delayed by a chain of register-only divisions (12 in the PoCs), so its verification at LQ completion is far behind the probe's cache request (`poc_v1_specgvq_1shot.c:223-266`; `poc_verify32.c:141-162`). When `L_a` completes, the actual value is the sentinel in the marker buffer, not the predicted secret; the model reports a value misprediction and squashes the younger window (`src/cpu/lvp/egdiff_predictor.cc:233-249`; `src/cpu/o3/lsq_unit.cc:1309-1318`). The probe's cache line has already been filled in gem5, and the fill is not rolled back with the squash. The attacker scans 256 lines with `rdcycle`; the hot line encodes the byte.

The estimator is calibrated per attempt. The architectural re-execution after the squash reads the sentinel and fills a reserved line outside the 256 measured lines; that line is a known-hot reference. The mean of the 256 measured lines is the cold reference. The threshold is the midpoint. A vote is cast only when exactly one measured line is below the threshold. A byte is accepted only with at least two votes and at least twice the runner-up's count; otherwise it is reported as unknown (`poc_v1_specgvq_1shot.c:288-352`; `poc_verify32.c:236-318`). This estimator was introduced after a naive "fastest line" estimator produced false positives when no speculative fill occurred: with all lines cold, the minimum of 256 miss latencies is a biased draw, and the harness fabricated bytes (`RESULTS_step1.md:8-45`). The current harness abstains rather than fabricates.

`known` is a **measurement mask, not a provenance certificate**. It means the estimator accepted a cache line for a byte; the oracle's exact-word comparison is the correctness test. A build with the key loads stubbed out can still report `known=ff` and eight zero bytes, which is `NO-MATCH`. This matters when reading the controls: the disabled-predictor run is meaningful because it is `known=00`, while the FLOP run's `known=ff` is its own constant-zero fill.

### 4.4 Mode 2: commit-time capture in the NVQ

A second, slower variant targets the non-speculative side. The victim executes a serializing `fence.i` before returning; the attacker then runs a capture body with exactly one constant-valued load at distance 1 from its own load. At commit, Algorithm 1 computes `delta_a = 0 - NVQ[1] = -S`, mismatches the stored diff, and latches `-S` into the attacker's table entry. A later replay with a zero base predicts `0 + (-S) = -S`. One capture is needed per byte, because the straight-line replay re-commits the sentinel and re-polls the entry (`plan.md:19-23`; `poc_v2_nvq_fence_bypass.c`; `IMPLEMENTATION.md:174-249`).

Mode 2 is a crafted-victim conformance experiment, not a real-target attack. Its `fence.i` is a harness ordering device: gem5 models `FENCE.I` as a pipeline drain, but on RISC-V `FENCE.I` orders instruction fetch, not data. The claim is narrower: the victim load has committed before the replay (guaranteed by the call/return and the harness fence), and the NVQ persists by definition. The experiment shows that the leak is not only a transient in-flight artifact; it can be written into the predictor table and replayed. It does not show a one-shot attack or a real victim.

### 4.5 Novelty relative to FLOP and Deng & Szefer

Deng and Szefer's value-predictor attacks require the attacker's load to alias the victim's predictor entry. In their Train+Test and Test+Hit attacks, the attacker trains or probes an index that the victim's secret-dependent access also maps to; the secret is inferred from whether the predictor's confidence or value was disturbed [Deng]. The mechanism is a collision between the attacker's and victim's predictor indices. EgDiff's attacker does not collide with the victim's table entry at all. It trains its own entry, and the victim's value is selected by its position in the untagged queue.

FLOP targets a local load-value predictor on Apple silicon [FLOP]. As with any cache-based observation, the predicted value must ultimately influence a memory access. EgDiff's attacker supplies that access in its own code: the probe is the attacker's load. The victim only has to load the secret; it does not have to branch on it, compare it, or use it as an address. The attacker still needs its own value-to-address probe, so "no gadget" means no gadget **in the victim**, not no gadget at all.

Our FLOP control in Section 6 uses the local constant-value predictor already implemented in the repository (`src/cpu/lvp/load_value_prediction_unit.*`). It is a specificity control: it shows that a local predictor in this harness does not forward the victim's value to the attacker's marker. It is not a reproduction of FLOP's attack, and it does not show that FLOP's attack would fail on EgDiff or elsewhere.

---

## 5. Methodology

### 5.1 Model and configuration

We use gem5's O3CPU in RISC-V (RV64) SE mode [gem5]. The attack runs use `configs/example/egdiff.py` with `--cpu-type=O3CPU --caches`: one core, 8-wide fetch/decode/dispatch/issue/commit, 192 ROB entries, 32-entry LQ and SQ, a 64 KiB 2-way L1D, a 32 KiB 2-way L1I, and a 2 GHz CPU clock (500 ticks per cycle). There is no L2 or L3 in the attack runs. The predictor is the model in `src/cpu/lvp/egdiff_predictor.{hh,cc}`, integrated through dispatch, deferred-wake, LQ-verification, commit-update, and ROB-squash hooks (`src/cpu/o3/iew.cc`, `lsq_unit.cc`, `commit.cc`, `rob.cc`).

The model differs from the paper's Table II in ways that matter for external validity. The paper models a 4 GHz core, a 32 KiB 4-way L1D, a 256 KiB L2, and a 2 MiB L3; we use 2 GHz, 64 KiB 2-way L1D, and no L2/L3 in the attack runs. The paper uses ARMv8-A and L-TAGE; we use RISC-V and TournamentBP. The paper uses 168/168 physical registers; we use 256/256. The paper uses SimPoints and 50 M warm-up + 100 M measured instructions; the SPEC smoke test uses the first 30 M from `_start`. These differences are acceptable for a mechanism demonstration but not for a performance or conformance claim.

The model implements the paper's logical structures: a 4,096-entry table with 14-bit tag, 2-bit `u`, 3-bit FPC, 64-bit `diff`, and 5-bit distance; a 32-entry speculative queue and a 32-entry NVQ; a 1/16 cold allocation; a 10-bit TICK; a 1,024-instruction `lastMisp`; and commit-stage distance polling (`src/cpu/lvp/egdiff_predictor.hh:44-69,116-120`; `src/cpu/lvp/egdiff_predictor.cc`). The C++ table entry is not bit-packed, but the logical fields and widths match the paper. The model does not implement the optional EVES-assisted early population, PRF port pressure, write elision, subword extension, or LQ sizes above 32.

### 5.2 Commit-based measurement definitions

The SPEC smoke test reports coverage and accuracy at commit. Coverage is `egdiffCommittedPredicted / egdiffCommits`: the fraction of committing loads whose prediction verified. Accuracy is `egdiffCommittedPredicted / (egdiffCommittedPredicted + numPredictionIncorrect)`. The numerator is commit-based; the denominator also includes wrong-path loads that verified incorrectly, so the two are not a strict partition. A correct prediction on a wrong-path load is in neither. The practical bias is small in the runs we inspected, but it is not zero in principle. Dispatch-time ratios are reported only as a diagnostic; on branchy code they are inflated by wrong-path loads.

### 5.3 Distance definition

The attack's `d` is the number of dynamic **load** instructions from the producer to the consumer, 1-based. If `n` loads execute strictly between the last sensitive load and the caller's marker, then `d = n + 1`. Stores, ALU operations, and branches do not advance the queue. In the predictor, `d = 1` means the base is the immediately preceding load. The prevalence study uses the same definition and reports the minimum over all sensitive PCs, using each PC's last execution before the marker (`prevalence/README.md`; `prevalence/results/prevalence.md`).

### 5.4 Prevalence method

The prevalence study runs host RISC-V binaries under `qemu-riscv64 -singlestep -d exec`, intersects the guest PC stream with the load PCs parsed from `objdump`, and computes `d` per call. The marker is the first load after the callee returns; the study checks `loads_since_ret == 0` in every row. Sensitive PCs are identified dynamically by protecting the key page with `PROT_NONE` and decoding each faulting load in a `SIGSEGV` handler; this yields the PC set and the per-call key-access count. A static argument-provenance analysis is used only as corroboration. Each target runs 300 calls per input mode (`msg`, `key`, `len` where applicable); the total is 6,300 calls over eight functions. The study reports the modal `d`, the mode frequency, the min/max, and the fraction of calls with `d ≤ 32`.

The method has limits. The QEMU trace is architectural, so it omits wrong-path loads that EgDiff's dispatch-order queue would see. The metric is a minimum over sensitive PCs, which is optimistic for functions with several sensitive loads. The fault probe over-approximates "reads the secret" for the synthetic controls: the winning PC can be a next pointer or a `used` flag. The 300 calls per mode replay a fixed input sequence, so they are not independent samples. The self-check covers only `d = 1`; an interior-distance self-check is still missing. And `d` is a property of the compiled binary, not of the source function.

### 5.5 Controls

The main controls are same-binary. The `EGDIFF_ON=0` run disables the predictor entirely; the `EGDIFF_TYPE=flop` run selects the local constant-value predictor already in the repository. Both run the same ELF, the same victim, and the same measurement loop; only the predictor changes. The A4 and SipHash gates assert that the disabled and FLOP runs recover zero bytes, that every `egdiff*` statistic is zero in the off run, and that the FLOP predictor is active (`run_layer6.sh:195-229`; `run_layer5.sh`). The Mode 2 gate additionally requires the off control to report `known=00`; the FLOP control is allowed to report `known=ff` with value 0, because its constant-zero prediction is a real fill, and is required to be `NO-MATCH` (`run_layer4.sh:96-111`).

The observation has three further controls. The address-delay sweep runs Mode 1 with 0, 1, 2, 3, 4, 6, 8, 10, and 12 register-only divisions. The zero-division row is a **partial** control: after fixing its address to read the sentinel, it recovers 3/8 bytes; one division recovers 8/8. It is not a clean falsifier. The branch-squash probe runs the same binary in `arch`, `squash`, and `none` modes; `none` is a null in which the wrong path is routed to a different line. The latency matrix runs Mode 1 with `(normal, deferred)` latency `(0,0)`, `(3,0)`, `(0,2)`, `(3,2)`, and `(5,2)`, plus an off control at `(3,2)`.

A4 and SipHash also have cold-key and varying-input variants. `COLD_KEY=1` removes the warm-up calls that make the key line L1-resident; `VARY_GUESS=1` changes the attacker's guess on every attempt (`poc_verify32.c:67-74,236-240`). These test whether the result depends on an undisclosed warm-up or a constant victim input.

### 5.6 Tick-to-cycle conversion

gem5's trace uses ticks. In our configuration the CPU clock is 500 ticks per cycle, so a `+3` publish is 1,500 ticks and the trace's timestamps can be converted by dividing by 500. The cycle margins in Section 6.6 are computed this way. This is a model clock, not a hardware clock.

---

## 6. Results

We separate three kinds of evidence. **Primary (A)** evidence directly supports a claim and has a control that could fail. **Supporting (B)** evidence strengthens a claim but does not establish it. **Weak (C)** evidence is an engineering sanity check or a presence check. **Not usable (D)** evidence is circular, biased, or does not measure the alleged quantity. Appendix A classifies every artifact. The main results use A and B only.

### 6.1 Model fidelity (B evidence)

The model is not a straw man. The Layer 1 trace shows Algorithm 1 on a three-load loop: allocation at distance 32, polling 32→31→…→2, latching `diff = 0x28` at `d = 2`, FPC saturation at 7, `u` saturation at 3, a stride change resetting `ctr` and `u`, polling 2→1, wrapping 1→32, and re-locking (`run_layer1.sh:71-105`; `IMPLEMENTATION.md:96-105`). Layer 2 shows one value misprediction for one induced bad stride, no forwarding in the 1,024-instruction window after it, forwarding resuming after it, and an exact architectural checksum on the branchy test (`run_layer2.sh`; `IMPLEMENTATION.md:106-112`). Layer 3 shows a deferred `Wake` that publishes when the base completes, an early-completion case where the binding is ignored, and a normal case at `d = 22` (`run_layer3.sh`; `IMPLEMENTATION.md:369-394`).

These are trace-presence assertions on small, crafted programs. They show that the model follows the paper's rules in the tested shapes; they do not prove equivalence to the paper's RTL (there is no RTL) or to an independent model (there is no co-simulation). The artifact reports 117 such assertions across six layers. We do not treat the count as evidence; several are presence checks, and the Layer 1 checks match values anywhere in the trace rather than for the tested load in the expected order. The value of the layers is qualitative: the model is close enough to the paper's described mechanism that the attack is not an artifact of a deliberately broken predictor.

The dispatch-order trace names the base of each prediction. In A4, for each key byte there are 16 predictions whose `base_val` is that byte; when the base was in the speculative queue, `base_pc` is the key-load PC, and when it had committed, `base_pc = 0` because the NVQ stores values, not PCs (`RESULTS_dispatch_trace.md:14-44`). This supports the claim that the slot at distance `d` holds the key byte and that no wrong-path load displaced it in this branchless target. The exact PCs in that note are from an earlier build; the distance map is stable, and the gate re-measures it.

### 6.2 Crafted forwarding: Mode 1 (A evidence)

The Mode 1 PoC uses a branchless victim that reads a 64-bit static value, uses it in registers, and scrubs the register before returning (`egdiff_secret.c:33-45`). The attacker trains its own load to `d = 1`, `diff = 0`, calls the victim once, and executes its load immediately after the return with an address delayed by 12 register-only divisions. The dependent probe encodes the forwarded byte into the cache.

The result is deterministic for `EGDIFF_SEED=1`: five different 64-bit values are recovered with `known=ff`, including two values composed of the estimator's sentinel bytes (`0xA5A5A5A55A5A5A5A` and `0x5A5A5A5AA5A5A5A5`), and the off and FLOP controls recover nothing with `known=00` (`RESULTS_step1.md:79-99`; `IMPLEMENTATION.md:161-170`). We re-ran the current gate: the default value `0x1122334455667788` is recovered with `known=ff`, and the off and FLOP runs are `NO-MATCH` with `known=00` (9/9 checks, `run_layer4.sh`). The five-value table is the stronger evidence because a harness that hard-coded one value would not pass the other four, and the alternating sentinels exercise the estimator's exclusion rule.

The control that matters is the off run: it is the same binary, the same victim, and the same measurement loop, and it produces zero informative attempts. The FLOP run is a second specificity control: the local predictor is active, but it does not forward the victim's value to the attacker's marker. The gate also verifies that the attacker translation unit has no reference to the secret symbol and that the secret is static (`run_layer4.sh:80-93`).

### 6.3 Crafted forwarding: Mode 2 (A for the NVQ path, B for realism)

The Mode 2 PoC uses a fenced victim and a capture body with one constant load at `d = 1`; Algorithm 1 latches `-S`, and a later replay with a zero base predicts `-S` (`poc_v2_nvq_fence_bypass.c`). The current gate recovers the full 64-bit value with `known=ff` and `arch=OK`; the off control is `NO-MATCH` with `known=00`; the FLOP control is `NO-MATCH` with `known=ff` and value 0 (`run_layer4.sh`; our re-run: 8/8 checks). The FLOP result is a genuine local-predictor fill, not a measurement artifact: the local predictor emits its own constant-zero prediction, the estimator sees a line, and the value is not the secret. The off result is the key control: after the `flush_drain()` fix, the disabled predictor produces no informative line at all. Before that fix, the off control emitted seven wrong bytes (`3f3f…00`, `known=fe`); the fix is recorded in `IMPLEMENTATION.md:206-249` and the current gate asserts `known=00` (`run_layer4.sh:101-103`). We re-ran the gate and confirmed `known=00`.

Mode 2 is a conformance experiment, not a real-target attack. It is crafted, it needs a serializing `fence.i` that gem5 treats as a drain but RISC-V does not treat as a data fence, it captures one byte per victim call, and it uses 16,800 victim calls (8 bytes × 7 attempts × 300 capture iterations). It shows that the leak can persist in the non-speculative table, not that it is practical. We classify it as A for the NVQ mechanism and B for external validity.

### 6.4 Real target: Monocypher `crypto_verify32` (A evidence, paper-faithful SpecGVQ)

The A4 target is `crypto_verify32` from Monocypher [Monocypher], a constant-time comparison called with the attacker's guess as the first operand and a 32-byte static key as the second (`target_verify32.c:40-56`). The attacker's translation unit never names the key. The gate builds the target with the pinned Monocypher source (commit `f8bb8dbd224556d58f5b4fa176c3f18fbfe50ebe`), checks the ownership split and the static load topology with `objdump`, and measures the key-byte distances in the freshly built binary with QEMU and `objdump` (`run_layer6.sh:46-148`).

The measured map is `24:13, 25:14, 26:12, 27:11, 28:10, 29:9, 30:8, 31:7`: the last 8-byte word of the key is at distances 7–14 from the caller's marker, with zero loads between the callee's `ret` and the marker. The attack recovers `key[24..31] = a1108f2ebd4cdb6a` with 8/8 bytes, 16 votes per lane, `known=ff`, and `arch=OK` (`RESULTS_A4.md:74-148`; our re-run of the gate: 36/36 checks, same word, same votes). The off control is `NO-MATCH` with `known=00` and every `egdiff*` statistic zero; the FLOP control is `NO-MATCH` with `known=00`; the FLOP predictor is active (`numPredictions > 0`).

The cold-key and varying-guess variants also recover 8/8 (`RESULTS_target_controls.md:7-29`). The baseline warms the key line with `target_verify32_selfcheck()` and two `target_verify32(guess)` calls; `COLD_KEY=1` removes them and still recovers the word, so the result does not depend on the warm-up in this model. `VARY_GUESS=1` changes the guess on every attempt and still recovers 8/8. We also re-ran the cold+varying combination and recovered 8/8 with 16 votes per lane; the `archpre` line reports `self=OK repro=OK marker=OK`, but in the `COLD_KEY` build `self_ok` and `repro` are set to 1 rather than measured (`poc_verify32.c:382-390`), so only the marker check is independent in that variant.

With the NVQ-base extension disabled (`EGDIFF_NVQ_BASE=0`), the same A4 gate still recovers 8/8: `egdiffNVQBases = 0`, every normal prediction is `base=specgvq`, and a nonzero `base_pc` names the key-load PC. The key load is still in the speculative queue when the marker dispatches, so this is a **paper-faithful SpecGVQ** result. The strict gate passes 36/36 at 12 divisions; the 1- and 4-division variants also recover 8/8, while 2 divisions is a measurement false negative. The default NVQ-base run also recovers, but it is not needed for the paper-faithful claim.

Each leaked byte requires a value misprediction at the marker (plus training noise) and triggers the 1,024-instruction `lastMisp` blackout, so the attacker must retrain between bytes. `RESULTS_A4.md` reports 369 incorrect predictions; our fresh run reports 355. The difference is build-dependent, and we report the range rather than a single exact number. The attack is loud in the model's statistics, even though it has no architectural output.

The A4 `sink` value is identical in the on, off, and FLOP runs, but it is **not** an architectural checksum: it is a harness accumulator that sums training values and the architectural sentinel probe and is identical by construction (`RESULTS_A4.md:178-185`). The gate calls it an "architectural checksum", but the artifact's own result note says it is not. The meaningful checks are `archpre` and the oracle's `MATCH`. Finally, the paper's +3/+2 latency is not on in the gated A4 run; the gate uses the default 0-cycle latency. We ran a supplementary A4 check with `EGDIFF_LATENCY=3` and `EGDIFF_DEFERRED_LATENCY=2` and recovered 8/8 with 16 votes per lane (command in Appendix B); that run is not a committed gate output, so we report it as supplementary. The committed latency evidence is the Mode 1 matrix in Section 6.6.

### 6.5 Real target: SipHash-2-4 on the empty-message path (A for the NVQ extension, B for paper fidelity, very narrow)

The SipHash target is Marek Majkowski's reference `siphash24` [SipHash], included essentially verbatim with `noinline/noclone/noipa` so that GCC cannot constant-propagate the empty message and delete the tail (`siphash_attack.c:1-39`; `egdiff_secret.c:61-139`). The key is a 16-byte static object in the target translation unit; the attacker calls `victim_tag()`, which returns only the tag. With `src_sz = 0`, the executed load stream is: a literal-pool round constant, `k0`, `k1`, three more round constants, the `switch` jump-table load, and the attacker's marker. The distances are `d(k1) = 5` and `d(k0) = 6`, both inside the order-32 window (`RESULTS_step2.md:76-122`; the gate re-checks the topology).

The default attack recovers `k0 = 0123456789abcdef` and `k1 = f1e2d3c4b5a69788`, 16/16 bytes, with 80/80 informative votes and `arch=OK` (`RESULTS_step2.md:22-58`; our re-run: 24/24 checks, same values, 80/80 votes). Its victim-phase bases are NVQ (`base_pc = 0`). With the NVQ-base extension disabled, SipHash **fails**: 0/16 at 1, 2, 4, and 12 divisions, and 0/16 with `COLD_KEY=1`. The result is therefore an **NVQ-extension** result, not a paper-faithful speculative-queue attack. The off control recovers 0/16 with `known=00` and every `egdiff*` statistic zero; the FLOP control recovers 0/16 with `known=00` and the FLOP predictor active. The cold-key variant also recovers 16/16 with NVQ enabled (`RESULTS_target_controls.md`).

This is the narrowest real-target result. It works only because the empty-message path has no message loads: with a non-empty message, the key loads are followed by hundreds of state and message loads, and `d > 32`. It also uses a five-attempt estimator with a fixed 20-cycle margin and a bare majority, weaker than the A4 estimator; 80/80 votes means every attempt produced a clear margin in this run, but one lucky attempt could decide a byte in a noisier setting (`SUBMISSION.md:441-443`). The result should be read as "the empty-message path is in-window and can be recovered", not "SipHash is broken".

### 6.6 Race, squash, and latency (A/B evidence for the observation)

The address-delay sweep is a controlled sensitivity, not a binary falsifier. With zero register-only divisions the attack recovers **3/8** bytes (`known=e0`); with one division it recovers all 8 (`known=ff`); 2, 3, 6, 8, 10, and 12 divisions also recover (`RESULTS_races.md:34-64`). The original zero row was invalid: its address was `atk_buf[1]`, not the sentinel, so the calibration line vanished. The corrected row shows a partial race at zero delay; the minimum for full recovery is one division. The 4-division run at 16 attempts lost one byte (`known=ef`) because that byte had zero informative attempts; at 32 attempts the same configuration recovered 32/32. That is a measurement false negative, not a latency cutoff: the estimator abstained rather than fabricating. The off controls at 0 and 12 divisions recover nothing.

The cycle trace gives the margin between the probe's cache request and verification. At one division, the request precedes verification by 54–185 cycles (average 62); at twelve divisions, by 115–331 cycles (average 197) (`RESULTS_races.md:66-82`). The trace's `+3` publish is 1,500 ticks, which is 3 cycles at 500 ticks per cycle. The probe is squashed before its completion is delivered, so the request tick is the relevant transient event; the fill can land later. This is a model measurement, not a hardware one.

The paper's latency matrix is `(0,0)`, `(3,0)`, `(0,2)`, `(3,2)`, and `(5,2)`. All recover 16/16 per byte except `(5,2)`, which recovers 8/8 bytes; the off control at `(3,2)` fails (`RESULTS_races.md:107-128`). We re-ran `(0,0)`, `(3,2)`, `(3,0)`, `(0,2)`, and the off control at `(3,2)` on the current build; `(3,2)` recovers and the off control fails. The pending-publish stage makes the normal +3 work: the earlier event-based attempt corrupted the run because the IQ marked the predicted destination ready at dispatch; the fix keeps it not-ready until the publish tick (`RESULTS_races.md:84-105`; `src/cpu/o3/iew.cc:505-660`). The matrix is evidence that the attack survives the paper's stated latency in this model, not evidence that the model reproduces the paper's pipeline.

The branch-squash probe runs with `EGDIFF_ON=0`, so no value prediction is involved. It trains a conditional branch, flushes the probe array, and then either executes the bad path architecturally (`arch`), mispredicts and squashes it (`squash`), or routes the bad path to a different line (`none`). `HOT_LINE` is 16 cycles in `arch`, 16 in `squash`, and 116 in `none`; the `none` null is cold even though the branch may speculate (`RESULTS_races.md:6-32`). A speculative cache fill therefore survives a branch-misprediction squash in this cache model; the survival is not specific to the value-misprediction path. It does not show that the fill survives on hardware.

### 6.7 Prevalence of the required code shape (B evidence)

The prevalence study measures the distance precondition, not the attack. Table 1 summarizes 6,300 calls over eight functions. End-loading comparisons and lookups satisfy `d ≤ 32` in 100 % of calls (`crypto_verify32` at `d = 7`; `memcmp` and `strcmp` at `d = 1`; the synthetic hash lookup and list walk at `d = 2` in 62.5 % of calls, 1–2 otherwise). Block primitives do not: `crypto_chacha20` has modal `d = 519` (min 85), keyed `blake2b` modal 552 (min 312), and `crypto_aead_lock` modal 1,339 (min 560), all outside the 32-load window (`prevalence/results/prevalence.md`).

| target | kind | calls | modal *d* | at mode | min–max | *d* ≤ 32 |
|---|---|---:|---:|---:|---:|---:|
| `crypto_verify32` | real Monocypher | 600 | 7 | 100 % | 7–7 | 100 % |
| `memcmp` | control implementation | 900 | 1 | 100 % | 1–1 | 100 % |
| `strcmp` | control implementation | 900 | 1 | 100 % | 1–1 | 100 % |
| hash lookup | control implementation | 600 | 2 | 62.5 % | 1–2 | 100 % |
| list walk | control implementation | 600 | 2 | 62.5 % | 1–2 | 100 % |
| `crypto_chacha20` | real Monocypher | 900 | 519 | 67.0 % | 85–579 | 0 % |
| `blake2b` (keyed) | real Monocypher | 900 | 552 | 67.2 % | 312–915 | 0 % |
| `crypto_aead_lock` | real Monocypher | 900 | 1,339 | 66.8 % | 560–1,462 | 0 % |

**Table 1.** Prevalence of the `d ≤ 32` precondition. The four "control implementation" rows are hand-written targets in `prevalence/targets/`, not the system libc. The modal `d` and the min–max are over calls; the 300 calls per mode replay a fixed input sequence. Source: `prevalence/results/prevalence.md` and `prevalence.csv` (6,300 rows).

The gap is large: in-window targets have `d ≤ 7` (at least 25 loads of margin), out-of-window targets `d ≥ 85` (more than 2.6× the window). The gap makes a wrong-path flip implausible for these functions, but it is empirical, not a proof. The block primitives are structural failures: they read the key once at entry and then execute hundreds of loads of derived state. The study's weaknesses are the small corpus (one real library, four hand-written controls), the over-approximating fault probe, the minimum-over-PCs metric (optimistic for lookup/walk), and the non-independent input modes. A `d ≤ 32` result does not mean an end-to-end attack was demonstrated.

### 6.8 Behavioural sanity on SPEC (C for conformance, B for "not broken")

The SPEC smoke test runs five SPEC CPU2017 refrate RISC-V binaries for 30 M instructions from `_start`, with commit-based coverage and accuracy. It is not a reproduction of the paper's 50 M warm-up + 100 M SimPoint windows, and it uses a different ISA, hash, cache hierarchy, and predictor latency (0 cycles). The five benchmarks are 505.mcf (64.48 % coverage, 99.99 % accuracy), 544.nab (27.48 %, 99.60 %), 548.exchange2 (17.35 %, 99.52 %), 510.parest (22.70 %, 99.83 %), and 531.deepsjeng (2.20 %, 95.16 %); the unweighted mean is 26.84 % and 98.82 %. These numbers are **not** comparable to the paper's steady-state coverage: `505.mcf`'s 64.48 % is startup array initialisation, and the paper's Fig. 10(b) shows its coverage in that evaluation below 10 %. The mean is unweighted, three benchmarks are excluded (`519.lbm`, `503.bwaves`, `557.xz`), `531.deepsjeng`'s first 30 M instructions are a hash-table zeroing loop, and the accuracy formula is not a strict commit-only ratio. Source: `spec_coverage/spec_f2.csv`. A paper-config variant (4 GHz, 32 KiB 4-way L1, 256 KiB L2, 2 MiB L3, L-TAGE, 168 registers, 192/64/32/32) gives 26.70 % / 98.77 % for the same five benchmarks (`spec_coverage/spec_f2_paper.csv`); the close agreement is a configuration sensitivity check, not a SimPoint reproduction. The test supports "the model is not broken", not "the model matches the paper".

---

## 7. Related work

**Value prediction.** Value prediction was introduced by Lipasti and Shen and by Lipasti, Wilkerson, and Shen, and its predictability was studied by Sazeides and Smith [Lipasti1996, Lipasti1996b, Sazeides1997]. Local predictors such as EVES are the state of the art in the CVP line [EVES]. Global value prediction was introduced by gDiff [gDiff]. EgDiff revisits gDiff with a single stride plus distance, commit-stage training, a forward probabilistic counter, and deferred prediction [EgDiff]. Our work does not propose a new predictor; it analyzes the security of EgDiff's queue.

**Value-predictor attacks.** Deng and Szefer were the first to analyze value predictors as a security problem. Their attacks require the attacker's access to alias the victim's predictor index; the secret is inferred from whether the predictor's confidence or value was disturbed [Deng]. FLOP demonstrates a load-value-predictor attack on Apple silicon [FLOP]. SLAP exploits a load-address predictor in the same family [SLAP]. Our mechanism differs in both dimensions: the attacker's table entry is its own (no collision), and the victim's value is selected by queue position (no victim-side value-to-address gadget). The attacker still supplies its own probe, so the difference is about where the gadget lives, not whether one exists.

**Spectre and transient execution.** Spectre uses control-flow speculation and a cache covert channel [Spectre]. Our observation is also a cache covert channel, but the trigger is a value misprediction, not a branch misprediction. The branch-squash control shows that the fill's survival is not specific to the value path in our model. The register-level forwarding would remain even if the cache observation were blocked.

**Defences and secure value prediction.** SafeSpec holds speculative fills in a shadow cache and installs them only at commit [SafeSpec]. InvisiSpec delays or buffers speculative memory state until it is safe [InvisiSpec]. Delay-on-Miss delays a speculative load that misses until it is non-speculative [DoM]. ISER recomputes values from a compiler-generated backward slice, so the value is exact and non-speculative [ISER]. Constable explores safe load elimination with value prediction [Constable]. These designs target the observation or the validation, not EgDiff's untagged base. A defence that only prevents speculative cache fills would leave the forwarded value usable in a register; whether that is exploitable depends on a channel we do not evaluate. We did not run any defence; the defence experiment is the main open item.

---

## 8. Limitations and threats to validity

**Capability boundary.** The threat model is a source-level non-disclosure convention, not an MMU or sandbox boundary. The attacker and victim share an address space; a native attacker could read the value directly. The `nm` check proves non-naming, not non-reading, and the attacker translation unit also references `target_verify32_selfcheck` and the oracle, which read the key. The experiment instantiates the convention, not a sandbox. This is the single most important limitation.

**gem5, not hardware.** All attack results are in gem5. The cache-fill survival is a property of gem5's cache model; the branch-squash control shows it is general to that model, but not that a real core keeps the fill. The predictor is our implementation of the paper's description, not the authors' RTL. The model differs in clock (2 GHz vs 4 GHz), L1D (64 KiB 2-way vs 32 KiB 4-way), L2/L3 (none vs 256 KiB/2 MiB), ISA (RISC-V vs ARMv8-A), branch predictor (TournamentBP vs L-TAGE), and registers (256/256 vs 168/168). Normal +3 and deferred +2 latency are modelled behind a parameter; the gated A4 and SipHash runs use 0-cycle latency, and the supplementary A4 3/2 run is not a committed gate output. PRF port pressure, write elision, the optional EVES-assisted population, subword extension, and LQ sizes above 32 are not modelled. There is no co-simulation against `egdiff_sim.py` on randomized streams; the model's fidelity rests on the layer traces and the paper quotes.

**Narrow real targets.** A4 is one 8-byte word of a 32-byte key in a branchless constant-time comparison; the full key is not recovered, and the two preceding words, although in-window in principle, were not demonstrated. SipHash is the empty-message path only; with a non-empty message, `d > 32`. The `memcmp`, `strcmp`, hash-lookup, and list-walk targets are prevalence measurements, not attacks. The real-target results are one library and one reference hash; they are not a survey. **A4 is a paper-faithful `SpecGVQ` result when the Rule C NVQ-base extension is disabled** (`EGDIFF_NVQ_BASE=0`; 8/8, `egdiffNVQBases=0`, `base=specgvq`). **SipHash is not**: with the extension off it fails 0/16, so its recovery is an NVQ-extension result. The paper's prediction text describes the speculative queue; the NVQ-base path is our transcribed Rule C extension, not the paper's predictor. The main paper-faithful evidence is crafted Mode 1 plus strict A4; SipHash belongs in the extension section.

**Mode 2 is crafted.** Mode 2 uses a fenced victim, a straight-line replay, one capture per byte, and 16,800 victim calls. Its `fence.i` is a harness ordering device, not a RISC-V data fence. It shows that the NVQ can be poisoned and replayed; it does not show a one-shot or real-target attack.

**Distance is build-specific and not a margin.** `d` is a property of the compiled binary, not the source function. A4's map was measured in this build; a codegen change can move it. The attacker needs offline knowledge of `d`, or a runtime discovery phase that tries the 32 positions; discovery is not demonstrated. Architectural `d` is not an upper or lower bound on dispatch `d` once wrong-path loads enter the window; A4 is branchless, and the `memcmp` probe has no branch in the window, so the measured agreement is not a positive control for divergence. The prevalence margin argument (in-window `d ≤ 7`, out-of-window `d ≥ 85`) is empirical.

**Estimator heterogeneity and loudness.** Mode 1/A4 uses per-attempt calibration, alternating sentinels, a 2-vote floor, and a 2× runner-up margin. Mode 2 uses a 3/7 vote floor and a 2× margin; SipHash uses five attempts, a fixed 20-cycle margin, and a bare majority. These are not one estimator; SipHash is the weakest. Each leaked byte costs a value misprediction, a squash, and a 1,024-instruction blackout; `numPredictionIncorrect` is in the hundreds. The attack has no architectural output but a loud microarchitectural signature; we did not study detection or throttling.

**`lastMisp` is an approximation.** The model counts instructions dispatched after the squash, not strictly committed correct-path instructions; a later wrong-path instruction can still consume slots before its own squash. The Layer-2 gate checks the blackout by sequence numbers, not by this counter.

**The 117-assertion count overstates the evidence.** Two of the checks are `sink`-equality sanity checks that are identical by construction and cannot fail; the effective evidence count is 115. Most of the remainder are trace-presence checks, not an equivalence proof.

**SPEC is a startup smoke test.** The 30 M-instruction window from `_start` has no warm-up; the paper uses 50 M warm-up + 100 M SimPoints. `505.mcf`'s 64.48 % is startup array initialisation; the paper's Fig. 10(b) shows its coverage in that evaluation below 10 %. The mean is unweighted, three benchmarks are excluded, and `531.deepsjeng` has 95.16 % accuracy. The accuracy formula is not a strict commit-only ratio. The smoke test supports "the model is not broken", not "the model matches the paper".

**No defence evaluation; victim behaviour is assumed.** SafeSpec, Delay-on-Miss, InvisiSpec, and ISER are not evaluated. They are expected to block the cache observation while leaving the register-level forwarding; whether the forwarding is exploitable without the cache observation is open. The impact claim is conditional on an undefended hierarchy. The victim's sensitive loads are assumed predictable or trainable enough for the queue to hold their values; a victim value misprediction would squash the window. The `VARY_GUESS` result suggests that a shared `load64_le` PC that alternates key/guess values does not lock, which helps here but is not a general robustness result.

---

## 9. Conclusion

EgDiff's global value queue is a shared, position-indexed, untagged store of recent load values, and its base selection is distance-only. That design choice is a cross-instruction forwarding primitive. An attacker who trains its own table entry to distance `d` and offset zero receives the value of whatever load sits `d` positions earlier, even when that load belongs to another software component, even when it writes no register the attacker reads, and even when the victim contains no value-to-address gadget. The primitive removes the table collision required by Deng and Szefer and the victim-side gadget required by FLOP. It does not remove the attacker's own value-to-address probe, and it does not create an MMU boundary where none exists.

In a gem5 model, under a same-process non-disclosure convention, the primitive recovers a crafted 64-bit value, the last 8-byte word of a Monocypher `crypto_verify32` key through the **speculative queue**, and the 16-byte key of SipHash-2-4 on the empty-message path through the **NVQ-base extension**. Same-binary controls with the predictor disabled and with a local constant-value predictor recover none of the secret. The required code shape occurs in end-loading comparisons and lookups (`d` = 1–7) and is absent from the block primitives we measured (`d` ≥ 85). The observation is a speculative cache fill that survives both value and branch squashes in gem5; the register-level forwarding would remain even if the fill were blocked.

The defensible claim is therefore a **design-level vulnerability analysis**, not a real-world exploit. The design fix follows from the mechanism: tag the queue base with a producer identity (for example, the producer PC) and require the consumer's table entry to expect that producer, or restrict predictions to bases produced by the load's own PC. The first preserves global prediction at the cost of a producer field in the table and the queue; the second is cheap but gives up the global correlation. We did not measure either fix. The open experiments are a defence evaluation, a hardware or second-model study, a full-key and non-empty-message attack, and a real non-disclosure boundary with runtime discovery of `d`.

---

## References

[EgDiff] L. Yang, L. Huang, Z. Zheng, B. Sui, S. Ma, Y. Wang, L. Shen, J. Wang, G. Chen, Q. Yang, S. Pei, and W. Xu, "Revisiting Global Value Prediction: A Resurgent Complement to Local Predictors," in *Proc. ISCA*, 2026.

[gDiff] H. Zhou, J. Flanagan, and T. M. Conte, "Detecting Global Stride Locality in Value Streams," in *Proc. ISCA*, 2003.

[EVES] A. Seznec, "Exploring Value Prediction with the EVES Predictor," in *Proc. CVP*, 2018.

[Lipasti1996] M. H. Lipasti and J. P. Shen, "Exceeding the Dataflow Limit via Value Prediction," in *Proc. MICRO*, 1996.

[Lipasti1996b] M. H. Lipasti, C. B. Wilkerson, and J. P. Shen, "Value Locality and Load Value Prediction," in *Proc. ASPLOS*, 1996.

[Sazeides1997] Y. Sazeides and J. E. Smith, "The Predictability of Data Values," in *Proc. MICRO*, 1997.

[Deng] S. Deng and J. Szefer, "New Predictor-Based Attacks in Processors," in *Proc. DAC*, 2021.

[FLOP] J. Kim, J. Chuang, D. Genkin, and Y. Yarom, "FLOP: Breaking the Apple M3 CPU via False Load Output Predictions," in *Proc. USENIX Security*, 2025.

[SLAP] J. Kim, D. Genkin, and Y. Yarom, "SLAP: Data Speculation Attacks via Load Address Prediction on Apple Silicon," in *Proc. IEEE S&P*, 2025.

[Spectre] P. Kocher, J. Horn, A. Fogh, D. Genkin, D. Gruss, W. Haas, M. Hamburg, M. Lipp, S. Mangard, T. Prescher, M. Schwarz, and Y. Yarom, "Spectre Attacks: Exploiting Speculative Execution," in *Proc. IEEE S&P*, 2019.

[SafeSpec] K. N. Khasawneh, E. M. Koruyeh, C. Song, D. Evtyushkin, D. Ponomarev, and N. Abu-Ghazaleh, "SafeSpec: Banishing the Spectre of a Meltdown with Leakage-Free Speculation," in *Proc. DAC*, 2019.

[InvisiSpec] M. Yan, J. Choi, D. Skarlatos, A. Morrison, C. Fletcher, and J. Torrellas, "InvisiSpec: Making Speculative Execution Invisible in the Cache Hierarchy," in *Proc. MICRO*, 2018.

[DoM] C. Sakalis, S. Kaxiras, A. Ros, A. Jimborean, and M. Själander, "Efficient Invisible Speculative Execution through Selective Delay and Value Prediction," in *Proc. ISCA*, 2019.

[ISER] C. Sakalis, Z. I. Chowdhury, S. Wadle, I. Akturk, A. Ros, M. Själander, S. Kaxiras, and U. R. Karpuzcu, "Do Not Predict – Recompute! How Value Recomputation Can Truly Boost the Performance of Invisible Speculation," in *Proc. SEED*, 2021.

[Constable] R. Bera, A. Ranganathan, J. Rakshit, S. Mahto, A. V. Nori, J. Gaur, A. Olgun, K. Kanellopoulos, M. Sadrosadati, and S. Subramoney, "Constable: Improving Performance and Power Efficiency by Safely Eliminating Load Instruction Execution," in *Proc. ISCA*, 2024.

[gem5] J. Lowe-Power et al., "The gem5 Simulator: Version 20.0+," *arXiv:2007.03152*, 2020.

[SipHash] J.-P. Aumasson and D. J. Bernstein, "SipHash: A Fast Short-Input PRF," in *Proc. INDOCRYPT*, 2012.

[Monocypher] L. Vaillant, "Monocypher," commit `f8bb8dbd224556d58f5b4fa176c3f18fbfe50ebe`, 2026.

---

## Appendix A — Artifact classification

**A** = primary evidence with a falsifiable control; **B** = supporting evidence; **C** = weak/presence-only or engineering sanity; **D** = not usable for the claim.

| Artifact | Claim it supports | Falsifier / control | Class |
|---|---|---|---|
| Layer 1–3 traces (`run_layer1.sh`–`run_layer3.sh`) | Model implements Algorithm 1, `lastMisp`, squash rollback, deferred `Wake` | Exact trace transitions; Layer 3 off control | B |
| 117 assertions (aggregate) | "Conformance" | None as a whole; many presence checks | C |
| Mode 1 crafted (`poc_v1`) | Untagged SpecGVQ forwarding of a 64-bit value | Off/FLOP same binary; five values; sentinels | A |
| Mode 2 crafted (`poc_v2`) | NVQ capture/replay persists the value | Off `known=00`; FLOP `known=ff` value 0, `NO-MATCH` | A (NVQ) / B (realism) |
| A4 `crypto_verify32` | Real library key word forwarded across a call | Off/FLOP 0/8; strict `EGDIFF_NVQ_BASE=0` 8/8 with `egdiffNVQBases=0`, `base=specgvq`; cold/vary 8/8 | A (paper-faithful SpecGVQ) |
| SipHash empty message | Real 16-byte key forwarded on one path | Off/FLOP 0/16; 80/80 votes; **strict SpecGVQ fails 0/16**; recovery needs NVQ | A (NVQ extension) / B (paper fidelity, narrow) |
| A4 cold/vary controls | Result not due to warm-up or constant input | Same binary, four variants; 8/8 | B |
| A4 latency 3/2 (supplementary) | Result survives the paper's stated latency | Off at 3/2 fails; not a committed gate output | B |
| Mode 1 latency matrix | Result survives 3/2 and 5/2 in the model | Off at 3/2 fails | A/B |
| Address-delay sweep | The transient race is real | 0 divisions partial (3/8); 1 recovers 8/8; off controls | A |
| Branch-squash probe | Fill survives a branch squash in gem5 | `none` null; `arch` positive; `squash` | A |
| Prevalence study | `d ≤ 32` occurs in end-loading functions; not in block primitives | 6,300 calls; 0 % for block primitives; fault probe | B |
| SPEC smoke test | Model is not broken on real binaries | Non-zero coverage, high accuracy | C (conformance) / B (sanity) |
| Dispatch trace (`RESULTS_dispatch_trace.md`) | Base slot holds the key byte | `base_val`/`base_pc`; one-call `memcmp` probe | B |
| `memcmp` d=1 probe | Architectural and dispatch `d` agree in a branchless window | Equal and early-exit cases | B |
| `sink` equality (A4 gate) | "Architectural checksum" | Identical by construction | D for checksum; C for gross divergence |
| `nm` non-naming | Attacker cannot name the key | No undefined key symbol; key static | C for boundary; B for convention |
| FLOP control | Leak is specific to EgDiff in this harness | Same binary, local predictor active, 0 bytes | B for specificity; C for FLOP claims |
| `egdiff_sim.py` / guess-invariance / lane map / Mode 2 fence / SipHash estimator | Reference model, distance stability, build mapping, ordering device, weak estimator | No co-simulation; measured per build; not a data fence; 80/80 but bare majority | C / B |

**What the artifact does and does not establish.** The A artifacts establish the mechanism and the specific recoveries in this model. The B artifacts strengthen the model's fidelity and the precondition. The C artifacts are engineering checks. The D artifact is the `sink` equality: it is not an architectural checksum, despite the gate's label. No artifact establishes hardware behaviour, a privilege boundary, a full key, a non-empty SipHash attack, a defence result, or a performance result.

## Appendix B — Exact commands

```bash
# Build the model (RISC-V)
scons build/RISCV/gem5.opt -j4

# Structural and end-to-end gates
tests/test-progs/egdiff_attack/run_layer1.sh
tests/test-progs/egdiff_attack/run_layer2.sh
tests/test-progs/egdiff_attack/run_layer3.sh
tests/test-progs/egdiff_attack/run_layer4.sh     # Mode 1 + Mode 2 + controls
tests/test-progs/egdiff_attack/run_layer5.sh     # SipHash + controls
tests/test-progs/egdiff_attack/run_layer6.sh     # crypto_verify32 + controls

# Mode 1 only / Mode 2 only
ONLY=1 tests/test-progs/egdiff_attack/run_layer4.sh
ONLY=2 tests/test-progs/egdiff_attack/run_layer4.sh

# SPEC smoke test (first 30M from _start)
python3 experiment/egdiff_eval/spec_coverage/run_spec.py \
    --maxinsts 30000000 --work /tmp/opencode/spec_f2

# Prevalence (host measurement; fetch the pinned Monocypher once)
cd experiment/egdiff_eval/prevalence
./tools/run_prevalence.sh

# Supplementary A4 run at the paper's stated latency (not a committed gate)
EGDIFF_TYPE=egdiff EGDIFF_ON=1 \
EGDIFF_LATENCY=3 EGDIFF_DEFERRED_LATENCY=2 \
build/RISCV/gem5.opt -d /tmp/opencode/a4lat \
    configs/example/egdiff.py --cpu-type=O3CPU --caches \
    --cmd="$PWD/tests/test-progs/egdiff_attack/poc_verify32" \
    --options="attack 24:13,25:14,26:12,27:11,28:10,29:9,30:8,31:7"

# Cold + varying A4 variant
MONO=experiment/egdiff_eval/prevalence/third_party/Monocypher/src
riscv64-linux-gnu-gcc -static -O2 -fno-pic -no-pie -fno-ipa-icf -I"$MONO" \
    -DCOLD_KEY=1 -DVARY_GUESS=1 -o /tmp/poc_a4_coldvary \
    tests/test-progs/egdiff_attack/poc_verify32.c \
    tests/test-progs/egdiff_attack/target_verify32.c "$MONO/monocypher.c"
EGDIFF_TYPE=egdiff EGDIFF_ON=1 build/RISCV/gem5.opt -d /tmp/opencode/a4coldvary \
    configs/example/egdiff.py --cpu-type=O3CPU --caches \
    --cmd=/tmp/poc_a4_coldvary \
    --options="attack 24:13,25:14,26:12,27:11,28:10,29:9,30:8,31:7"
```

The A4 lane map is measured per build by `tests/test-progs/egdiff_attack/measure_d.py`; the map above is the one we measured. The gate re-measures it and fails if the topology changes.

---

## Appendix C — What a skeptical reader should not infer

1. **No hardware vulnerability.** Every attack result is gem5; the cache-fill survival is a model property. We did not run on hardware.
2. **No privilege or cross-process leak.** Single core, single thread, same address space; the non-disclosure convention is not an MMU boundary.
3. **No non-reading claim.** A native same-process attacker can read memory directly. The `nm` check proves non-naming, not non-reading.
4. **No full key recovery.** One 8-byte word of a 32-byte key; empty-message SipHash only.
5. **No silent or offline-free attack.** Each byte costs a squash and a 1,024-instruction blackout; `d` is measured offline, and runtime discovery is not demonstrated.
6. **No conformance proof from 117 assertions, and no checksum from `sink`.** The assertions are mostly trace-presence checks; `sink` is a harness accumulator, identical by construction.
7. **No FLOP attack reproduction.** The FLOP control is the local predictor already in the repository; it is a specificity control, not FLOP's attack.
8. **No paper reproduction from SPEC.** The 30 M from `_start` smoke test uses a different ISA, cache hierarchy, and window; the paper's steady-state numbers differ.
9. **No defence result and no measured fix.** SafeSpec/DoM/InvisiSpec/ISER were not evaluated; tagging the base and restricting cross-PC forwarding are recommendations.
10. **No generalization from the narrow targets.** A4 is one word of one key in a branchless compare; SipHash is the empty-message path.

**Build dependence, not a claim change.** `RESULTS_A4.md` lists key-load PCs from an earlier build (`0x10f58`, `0x10f7e`); the fresh build measures `0x10f60`, `0x10f86` for the same `d` map. `RESULTS_step2.md` already notes the pre-refactor 22/22 and 79/80 counts and the current 24/24 and 80/80. The conclusions are stable; exact PCs and vote counts are build-dependent. We use current gate results where we re-ran them and say so; otherwise we cite the committed file. Monocypher is pinned but not vendored; SPEC binaries are external. The working tree has an uncommitted `src/mem/cache/base.cc` change from a separate SafeSpec experiment; it only alters `isShadowNoAllocate` packets, which the EgDiff runs do not generate, so it should not affect the numbers, but the tree is not byte-identical to a clean checkout.

## Appendix D — Missing experiments, ranked by value

1. **Defence evaluation.** Run SafeSpec, Delay-on-Miss, InvisiSpec, and/or ISER on the A4 and SipHash harnesses. This is the highest-value experiment because it decides whether the result is a cache-observation artifact or a register-level capability. Also test a register-only consequence (for example, a branch whose direction depends on the forwarded value) to see whether the forwarding is exploitable without a cache fill.
2. **A real non-disclosure boundary.** Instantiate a sandbox or JIT boundary that actually prevents the attacker's code from reading the target's memory, and show that the predictor crosses it. Alternatively, state clearly that the current result is a convention and measure what a native attacker would do instead.
3. **Broader real targets.** Non-empty SipHash; the full 32-byte `crypto_verify32` key; other constant-time comparisons and MACs; a real application call path.
4. **Hardware or a second model.** A hardware demonstration, or at least an independent simulator, would test whether the cache fill survives outside gem5 and whether the +3/+2 latency changes the race. Co-simulation against `egdiff_sim.py` on randomized streams would test the model's equivalence.
5. **Microarchitectural fidelity.** LQ = 48/64; subword sign/zero extension; EVES-assisted early population; PRF port pressure and write elision; 4 GHz and the paper's 32 KiB 4-way L1D/L2/L3.
6. **Prevalence at scale.** More libraries, automated detection of `d ≤ 32` shapes, and a study of how often an attacker controls the call site.
7. **Attack cost and detectability.** Quantify the squashes, the 1,024-instruction blackout, and the timing; study whether a monitor could detect the pattern.
8. **End-to-end application.** A MAC over a chosen message, or a real sandbox helper, would test the external validity of the threat model.

Until at least (1) and (2) are done, the honest framing is the one in this paper: a design-level vulnerability analysis in a model, with a plausible fix and a clear path to stronger evidence.
