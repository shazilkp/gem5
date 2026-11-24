# EgDiff in gem5 — Implementation Status & Reviewer Guide

Companion to `plan.md` (the specification). This file is the **live implementation
record**: what is built, exactly where it is hooked into the O3CPU, how it deviates
from `plan.md`, how each layer is verified, and what a reviewer should check.

- **Branch:** `feat/egdiff-attack`
- **Simulator:** `build/RISCV/gem5.opt` (RV64)
- **Test compiler:** `riscv64-linux-gnu-gcc -static -O1` (Layer tests) / `-O2` (attack PoCs)
- **Reference model (approved):** `experiment/egdiff_eval/egdiff_sim.py`
- **Spec:** `experiment/egdiff_eval/plan.md`
- **Paper:** `docs/papers/Revisiting_Global_Value_Prediction_A_Resurgent_Complement_to_Local_Predictors.pdf`

Everything below is **single-core, single-thread** (matching the paper and FLOP).

---

## 1. Layer status

| Layer | Scope | Status | Test |
|---|---|---|---|
| **1** | Commit-stage engine: 4K×88-bit table, FPC, TICK decay, 1/16 allocation, NVQ, Algorithm 1 | **DONE — 17/17 checks** | `tests/test-progs/egdiff_attack/run_layer1.sh` |
| **2** | `SpecGVQ` lifecycle, dispatch prediction, LQ verification + squash, `lastMisp` blackout, ROB rollback | **DONE — 10/10 checks** | `tests/test-progs/egdiff_attack/run_layer2.sh` |
| **3** | Deferred prediction (`Wake`/`Avail` binding, one-to-one) + asynchronous register wakeup | **DONE — 13/13 checks** (auditor-verified) | `tests/test-progs/egdiff_attack/run_layer3.sh` |
| **4** | End-to-end experiments (`SpecGVQ` in-flight, `NVQ` capture + replay) | **DONE — 14/14 checks** | `tests/test-progs/egdiff_attack/run_layer4.sh` |
| **5** | Real library target (SipHash-2-4, 128-bit key) | **DONE — 24/24 checks** | `tests/test-progs/egdiff_attack/run_layer5.sh` |
| **6 / A4** | Real Monocypher `crypto_verify32` target in gem5 | **DONE — 36/36 checks** | `tests/test-progs/egdiff_attack/run_layer6.sh` |

**Regression status:** with `EGDIFF_ON=0` and with `EGDIFF_TYPE=flop`, all three
Layer test binaries still produce exact architectural checksums and every
`egdiff*` statistic is zero (the predictor is fully inert). Layers 1–3 pass after
the Layer 3 changes.

---

## 2. Files added / changed

### New
| File | Purpose |
|---|---|
| `src/cpu/lvp/egdiff_predictor.hh` | EgDiff engine: `EgDiffTableEntry` (88-bit), `EgDiffSpecEntry` (141-bit GVQ slot), `EgDiffPredictor` API, result structs. Deliberately **free of O3CPU includes**. |
| `src/cpu/lvp/egdiff_predictor.cc` | `commitLoad` (Algorithm 1 + TICK + NVQ), `dispatchLoad`, `completeLoad`, `squashFrom`, `retireFront`, `reset`. |
| `configs/example/egdiff.py` | Selects EgDiff on every CPU; forwards all `se.py` options. |
| `tests/test-progs/egdiff_attack/test_layer1_polling.c` + `run_layer1.sh` | Layer 1. |
| `tests/test-progs/egdiff_attack/test_layer2_spec_and_squash.c` | Layer 2 part 1 (clean stride + one induced misprediction). |
| `tests/test-progs/egdiff_attack/test_layer2_branch_squash.c` | Layer 2 part 2 (wrong-path loads discarded by ROB squash). |
| `tests/test-progs/egdiff_attack/run_layer2.sh` | Layer 2 driver. |
| `tests/test-progs/egdiff_attack/test_layer3_deferred_wake.c` + `run_layer3.sh` | Layer 3 (three deferred/normal sub-cases). |
| `tests/test-progs/egdiff_attack/poc_v1_specgvq_1shot.c` + `run_layer4.sh` | Layer 4 Mode 1 (1-shot teleportation) and its controls. |
| `tests/test-progs/egdiff_attack/poc_v2_nvq_fence_bypass.c` | Layer 4 Mode 2 (fenced-helper NVQ capture + replay) and its controls. |
| `tests/test-progs/egdiff_attack/siphash_attack.c` + `run_layer5.sh` | Layer 5 (real SipHash-2-4 target) and its controls. |

### Modified
| File | Change |
|---|---|
| `src/cpu/lvp/LoadValuePredictionUnit.py` | New params: `predictorType` (`"flop"`/`"egdiff"`), `egdiffTableEntries`, `egdiffSeed`, `egdiffColdDeterministicAlloc`. |
| `src/cpu/lvp/SConscript` | Adds `egdiff_predictor.cc` and the `EgDiff` debug flag. |
| `src/cpu/lvp/load_value_prediction_unit.hh/.cc` | Owns `std::unique_ptr<EgDiffPredictor>`; O3-free glue (`commitLoad`, `dispatchLoad`, `completeLoad`, `squashFrom`, `retireLoad`); EgDiff statistics. `predictLoad` returns invalid in EgDiff mode; CVU store-invalidation is skipped in EgDiff mode. |
| `src/cpu/o3/commit.cc` | Commit hook: `cpu->lvp->commitLoad(...)` for every non-faulting committing load — the **only** writer of the prediction table. |
| `src/cpu/o3/iew.cc` | Dispatch hook: `dispatchLoad(...)` for every load; forwards the value to the PRF + scoreboard when valid. Adds `IEW::lvpWakeDeferred()`. |
| `src/cpu/o3/inst_queue.hh/.cc` | Adds `InstructionQueue::wakeRegDependents(DynInst*)` (register-only wakeup) and refactors `wakeDependents` to call it. |
| `src/cpu/o3/lsq_unit.cc` | EgDiff completion hook: `completeLoad(...)`; squash on mismatch; deferred wake via `iewStage->lvpWakeDeferred`. |
| `src/cpu/o3/rob.cc` | `ROB::squash` calls `cpu->lvp->squashFrom(squash_num)` (speculative queue only). |

---

## 3. Where each hardware stage is hooked

1. **Dispatch (`iew.cc`, inside `else if (inst->isLoad())`)** — for each load:
   `dispatchLoad(seqNum, pc, inst)`. On `valid`, `cpu->setReg(destReg, value)` +
   `scoreboard->setReg(destReg)` (dependents issue immediately). Every candidate
   load gets a `SpecGVQ` slot even with no prediction.
2. **Completion (`lsq_unit.cc`, `LSQUnit::writeback`)** — after `completeAcc`:
   `completeLoad(seqNum, actualVal)`. On mismatch → `squashDueToMemOrder`.
   If a deferred dependent is returned → `IEW::lvpWakeDeferred(dep, value)`.
3. **Deferred wake (`iew.cc`, `IEW::lvpWakeDeferred`)** — writes PRF, sets the
   scoreboard bit, `setLVPPrediction`, and `instQueue.wakeRegDependents(inst)`.
   It **does not** complete the memory access (see §5.3).
4. **Commit (`commit.cc`)** — `commitLoad(seqNum, pc, actualVal)`; then
   `retireFront(seqNum)` pops the committed slot.
5. **Squash (`rob.cc`)** — `squashFrom(squash_num)` drops every slot with
   `seqNum > squash_num`, clears stale `wakeSeqNum`, and leaves the table + NVQ
   untouched.

---

## 4. Verification

### Commands
```bash
scons build/RISCV/gem5.opt -j4
tests/test-progs/egdiff_attack/run_layer1.sh
tests/test-progs/egdiff_attack/run_layer2.sh
```

### Layer 1 result — PASS (17/17)
`--debug-flags=EgDiff` trace of a 3-load loop (`A` → `C` → `B`, `B = A + 40`,
dynamic distance 2) shows, for the `B` load:
- 1/16 allocation after a tag mismatch with `u == 0`;
- `distance` polling `32 → 31 → … → 2`, latching `diff = 0x28` on `3 → 2`;
- FPC `0 → 1` deterministic then probabilistic to `ctr = 7`; `u → 3`;
- one stride change (`+99`) resets `ctr = 0`, `u = 0`, polls `2 → 1`, wraps
  `1 → 32`, re-locks at `2` with `diff = 0x63`;
- TICK decay count matches `egdiffCommits / 1024`.

### Layer 2 result — PASS (10/10)
- Exact architectural checksum after squashes (clean test).
- Exactly **one** value misprediction for the single induced bad stride.
- No value forwarded for `sn ∈ (M, M+1024]`; forwarding resumes after `M+1024`
  (counted over both normal and deferred paths).
- `egdiffLastMispSuppressed > 0`.
- Branchy test: `egdiffSquashed > 0` with exact checksum.

> **Test-design note (important).** A naive expected-value recomputation loop
> introduced a *second* legitimate misprediction: any loop that reads both the
> base and target arrays at a fixed relation trains its own entry, and the
> induced bad element then mismatches there too (observed: an `A`-load training
> at `d = 1` with `diff = -40`). The test therefore computes the expected sum
> *before* the bad store and adds the `+59` delta explicitly.

> **Observation for Layer 4 (attack relevance).** In tight loops the base load
> is almost always still in flight when the dependent dispatches, so *every*
> Layer 2 prediction fired through the deferred (`Wake`) path
> (`egdiffPredictions = 0`, `egdiffDeferredPredictions > 1500`). Mode 1B of the
> attack (deferred `SpecGVQ` teleportation) is therefore the natural fast path,
> not an edge case.

### Layer 4 (Mode 1) — PASS (7/7 for Mode 1): 1-shot in-flight `SpecGVQ` teleportation
`poc_v1_specgvq_1shot.c`, gated by `run_layer4.sh`. Measurement hardening is
documented in `RESULTS_step1.md`.

Construction:
- The victim is `noinline, noclone, noipa`, branchless, performs no
  secret-dependent memory access, and overwrites its secret register before
  returning. It reads the secret itself so the caller inserts no load between
  the secret load and `L_atk`.
- `L_atk` is one function (`atk_load`) used in both training and the attack, so
  the PC is identical. Training uses pairs `(p, p)`, so `diff == 0` is constant
  only at `d == 1`; distance polling therefore lands on 1 (pairs avoid the
  self-stride trap).
- Built `-fno-pic -no-pie` and with the array base passed as an argument so that
  addressing a global never inserts a hidden load (which would become the
  prediction base).
- `L_atk`'s address is derived from 12 register-only divisions (no loads), so it
  issues long after the prediction; the dependent probe load issues immediately
  with the predicted secret.
- After training, `atk_buf[0]` is overwritten with a **sentinel** (a store does
  not train the predictor). When the mispredicted `L_atk` is re-executed after
  the squash, its architectural probe fills the sentinel line, which is used as
  a **per-attempt hot/cold calibration reference** rather than a vote.
- The estimator requires exactly one non-sentinel line below the calibrated
  threshold, alternates two sentinels (`0xA5…` / `0x5A…`) so a byte equal to a
  sentinel byte is still measurable, and reports an unresolved byte as
  `UNKNOWN` (`known` bit clear) rather than fabricating one. The verdict
  requires `known == 0xff`, so a partial measurement cannot be reported as a
  success.
- A `fence.i` between the eviction loop and the helper call drains the
  eviction's outstanding misses; without it their L1D MSHR occupancy can starve
  the transient probe (measured 1/128 tries vs 127/128).

Result (deterministic; each row one run):
```
EgDiff on  : 0x1122334455667788 MATCH known=ff
             0xDEADBEEFCAFEBABE MATCH known=ff
             0xFFFF0000FFFF0000 MATCH known=ff
             0xA5A5A5A55A5A5A5A MATCH known=ff   (both sentinel bytes)
             0x5A5A5A5AA5A5A5A5 MATCH known=ff   (both sentinel bytes)
EgDiff off : recovered=0000000000000000 known=00 NO-MATCH
FLOP LVP   : recovered=0000000000000000 known=00 NO-MATCH
```
Stats for the EgDiff run: deferred predictions in the 10^5 range,
`numPredictionIncorrect > 0` (the measured loads, as expected).

### Layer 4 (Mode 2) — PASS (conformance experiment, after review fixes)
`poc_v2_nvq_fence_bypass.c`, gated by `run_layer4.sh`.

The first version of this experiment was invalid; an independent review
identified the causes (kept in the review record below). The corrected design:

- **One fixed point.** The capture body is 31 varying noise loads, then the
  helper's single constant load, then the attacker load. The only distance
  whose difference is constant is `d = 1`; polling walks `32 → … → 2` and the
  `2 → 1` step latches `diff = 0 − S = −S`. (The earlier 3-load body had a
  fixed point at every `d ≡ 0, 1 (mod 3)` and locked at `d = 31`.)
- **One capture per byte.** A straight-line replay lets the re-executed load
  commit and re-poll the entry, so the entry is re-captured for each byte.
- **Dedicated architectural probe slot.** The transmitter routes the
  architectural (post-squash) value to probe slot `LINES`, outside the measured
  range, so no byte index has to be excluded.
- **Strict verdict.** A byte is accepted only if the fastest line beats the
  runner-up by a margin, and the harness checks an architectural precondition
  (`victim(0) == 0`, sentinel in place). It reports `arch=OK/BAD`.

Result (deterministic):
```
EgDiff on  : recovered=1122334455667788 arch=OK MATCH
EgDiff off : recovered=3f3f3f3f3f3f3f00 arch=OK NO-MATCH
FLOP LVP   : recovered=0000000000000000 arch=OK NO-MATCH
```

Caveat recorded: the helper executes `fence.i` (which gem5 models as
serialising). The original `fence rw, rw` is *not* a pipeline drain in gem5, so
phrasing the result as "survives a full serialising fence" is only accurate for
`fence.i`.

### Layer 4 (Mode 2) — review record (why the first version was invalid)
An independent conformance review established that the **simulator model is
conformant** to Algorithm 1 and the NVQ ordering, and identified the harness
defects that were then fixed. The review's key points, retained for the record:

- **The simulator model is conformant.** The commit-stage `Algorithm 1`, the
  NVQ write ordering, and the sole-writer property all match the paper; the
  trace shows `diff = -S` being latched through `nvq[]` at commit.
- **The experiment did not test the documented behaviour.** Root causes:
  1. The capture body had 3 loads (varying pair, secret, attacker load = 0), so
     **every** distance `d ≡ 0 (mod 3)` is a fixed point with `diff = 0` and
     **every** `d ≡ 1 (mod 3)` is a fixed point with `diff = -S`. Cold
     allocation starts at `d = 32 ≡ 2`, mismatches, and lands on `d = 31 ≡ 1`,
     where it locks. The required `d = 2` state is never established, so the
     `2 → 1` latch (the only transition that stores `-S`) never runs. `d = 1`
     was reached only incidentally after repeated replays.
  2. The replay was straight-line, so after the misprediction the re-fetched
     load **committed** the sentinel and re-polled the entry
     (`31 → 30 → 29` with `diff` becoming 0), after which the entry predicted
     zero. Plan.md's branch-shadow replay is required to preserve the entry.
  3. The sentinel line was excluded incorrectly: the architectural path indexes
     the probe with `-SENTINEL`, so its bytes are `0x56`/`0x55`, not `0xAA`.
     The "recovered" value was therefore the architectural sentinel path, and
     the disabled/FLOP controls also printed it.
  4. The verdict had no architectural checksum and could report a byte for free
     (no requirement that the winning line had any votes; secret bytes equal to
     a default/hot line match trivially).
  5. `fence rw, rw` is **not** a pipeline drain in gem5 (it is not
     `IsNonSpeculative`/`IsSerializeAfter`/`IsSquashAfter`), so "survives a full
     serialising fence" is not demonstrated by that instruction.

Fix plan (from the review), in order:
  1. Make `d = 1` the only fixed point: a capture body of >32 loads with exactly
     one constant-valued load (the secret) at the `d = 1` slot, everything else
     varying per iteration. Then polling walks `32 → … → 2` and the `2 → 1`
     step latches `-S`.
  2. Either add the branch-shadow replay from plan.md §4.2, or accept one
     capture per byte (re-capture after each replay).
  3. Give the architectural path a **dedicated probe slot outside the measured
     range** (e.g. select slot `LINES` when the register still holds the
     sentinel) instead of trying to exclude a byte index.
  4. Require a strict margin and a real architectural checksum in the verdict.
  5. Use `fence.i` if the fence-drain property is part of the claim, else scope
     the claim down.

### Layer 5 — PASS (22/22): real SipHash-2-4 target
`tests/test-progs/egdiff_attack/siphash_attack.c`, gated by `run_layer5.sh`.

- Uses the canonical `majek/csiphash` reference `siphash24()` verbatim (only the
  `uint8_t` signature and a `noinline/noclone/noipa` annotation, so the compiler
  cannot specialise away the zero-length reference tail).
- With `src_sz = 0`, `objdump` gives `d(k1 → L_atk) = 5` and `d(k0 → L_atk) = 6`
  dynamic loads; the four intervening loads are literal-pool constants and the
  `switch` jump-table load, and there is no load between the `ret` and `L_atk`.
  The gate re-checks that topology so codegen drift cannot silently change the
  base.
- Result: `k0 = 0123456789abcdef`, `k1 = f1e2d3c4b5a69788`, recovered 16/16 bytes
  (80/80 informative votes after the code-ownership split). `EGDIFF_ON=0` and
  `EGDIFF_TYPE=flop` both recover
  0/16 with every EgDiff statistic zero and the same architectural checksum.
- **Independently verified**: the implementation passes the published
  SipHash-2-4 known-answer vectors (`len 0 → 0x726fdb47dd0e0e31`,
  `len 1 → 0x74f839c593dc67fd`, `len 15 → 0xa129ca6149be45e5`) when linked
  against a separately built test harness.

Caveats: the harness deliberately touches the key line immediately before the
SipHash call, so the base is L1-resident and available at `L_atk` rename. Layer 5
therefore exercises the base-available/committed-NVQ forward path; the deferred
path is covered by Layer 3 and Mode 2.

### F2 — behavioural conformance on SPEC CPU2017
`experiment/egdiff_eval/spec_coverage/` (`run_spec.py`, `README.md`).

Method: static RISC-V SPEC CPU2017 refrate binaries, O3CPU + `--caches
--l2cache`, 30M-instruction window, coverage and accuracy measured **at commit**
(a prediction counts only if its load commits). The commit-time accounting was
necessary: `557.xz` dispatches 1.78M predicted loads against 0.20M committed
ones because of ~2M branch-misprediction squashes, so dispatch-time ratios are
meaningless on branchy code.

| benchmark | committed loads | coverage | accuracy |
|---|---|---|---|
| 505.mcf | 6,426,344 | 64.51% | 99.99% |
| 544.nab | 5,527,140 | 28.38% | 99.60% |
| 548.exchange2 | 4,951,309 | 17.52% | 99.49% |
| 510.parest | 5,161,173 | 22.84% | 99.83% |

Mean over the four: **33.3% coverage, 99.73% accuracy**; excluding `505.mcf`
(pointer chasing — the paper's own motivating shape — hence high coverage), the
remaining three average **22.9% coverage**, inside the paper's reported 22–26 %,
with accuracy 99.5–99.8 % against the paper's >99 %.

Excluded, with reasons recorded in the README: `519.lbm` (SE register-only spin),
`503.bwaves` (exits after ~138k instructions), `557.xz` (squash-heavy, 8x slower),
`531.deepsjeng` (first 30M instructions are a 720 MB hash-table zeroing loop).

**Conclusion for F2:** the model behaves like the paper's predictor in the
aggregate — high accuracy and coverage in the expected band. It is not a
reproduction of the paper's per-benchmark numbers (different ISA, hash, memory
hierarchy, window length, no SimPoints).

### A4 / Layer 6 — real Monocypher target in gem5 (PASS, 36/36)
`tests/test-progs/egdiff_attack/{poc_verify32.c,target_verify32.c,measure_d.py,run_layer6.sh}`,
results in `RESULTS_A4.md`.

The prevalence study said end-loading check functions are structurally eligible;
this experiments confirms it on a real library. `target_verify32.c` owns a
32-byte key as a `static const` object and exposes only
`target_verify32(guess)`, which calls the real `crypto_verify32(guess, key)`.
The attacker TU has no key reference (`nm -u` clean; the key is a local `r`
symbol).

`measure_d.py` re-derives the distances in the freshly built binary with
`qemu-riscv64 -singlestep` + `objdump` (not taken from the prevalence table):
the last key word's byte loads are at **d = 7..14**, with **zero loads between
the callee `ret` and the marker**. The attacker trains at each measured `d`
(generalised from the Mode 1 `d=1` pair training) and recovers the word via the
cache probe.

Result: **8/8 bytes** of key[24..31] = `a1108f2ebd4cdb6a`, 16 votes per lane,
`known=ff`. `EGDIFF_ON=0` and `EGDIFF_TYPE=flop` both recover 0/8 with
`known=00`, every EgDiff statistic zero, the FLOP predictor active, and an
identical architectural checksum. The gate (`run_layer6.sh`) re-checks the
ownership split, the distance map, the adjacent-marker property, the recovery
and both controls: **36/36**.

#### Dispatch-order confirmation (closes the architectural-trace caveat)
The EgDiff trace now records every slot allocation (`alloc sn=.. pc=..`) and, on
each prediction, the slot's value and PC (`base_val`, `base_pc`). Running the A4
attack with `--debug-flags=EgDiff` shows, for each of the eight key bytes, 16
predictions whose `base_val` is that byte, and whose `base_pc` is the key-load PC
when the base was still in the speculative queue (`0x10f58` and `0x10f7e` are
`lbu` key loads). A one-call `memcmp` probe (`memcmp_probe.c` +
`measure_dispatch_d.py`) shows architectural and dispatch `d` agree at 1 for both
the equal and early-exit cases. See `RESULTS_dispatch_trace.md`. In-window
prevalence targets have `d <= 7`, so the architectural lower bound leaves >= 25
loads of margin against wrong-path pollution.

Caveats recorded in `RESULTS_A4.md`: distances come from QEMU's architectural
trace (there is no branch between the last key load and the marker, so dispatch
order matches); lane offsets are public code structure, only the key values come
from the oracle; `crypto_chacha20` (modal d = 519) is structurally out of range
and recorded rather than re-run.

### Code ownership and the threat model (F2)
The target value (Layers 4) and the SipHash key (Layer 5) live in a separate
translation unit, `egdiff_secret.c`, as `static` objects. Only the helper
(`victim_value`, `victim_value_fenced`, `victim_tag` -> `siphash24`) and the
post-hoc `oracle_report_*` functions read them; the oracle plays the role of the
experimenter checking the answer and runs after the measurement.

The attacker translation unit therefore has **no architectural reference** to the
value it is supposed to obtain. This is checked mechanically by the gates:
`nm -u` on the attacker object shows no `secret_` symbol, and the linked binary
has no global `secret_*` symbol (they are local). The claim is that the attacker's
instruction stream never loads the value, so the only way it reaches an attacker
register is the predictor. This is the same-process "cannot name it" model used
by FLOP's engine boundary, **not** an MMU-enforced isolation claim.

### Layer 3 result — PASS (13/13, auditor-verified)
Three independent phases, each with its own load PCs:
- **A (active wakeup):** base and dependent stream at a 4 KiB stride (both miss
  L1D); the dependent binds `Wake` while the base is unresolved and is woken when
  the base completes. Verified by `deferred wakeup -> publish … (d=2)` in the
  trace and by `egdiffDeferredPredictions > 0`.
- **B (early-completion guard):** base streams (miss) but the dependent is hot, so
  it completes first and the binding is discarded: `deferred wakeup ignored` and
  `egdiffDeferredIgnored > 0`.
- **C (normal dispatch path):** 21 intervening loads (`d = 22`) so the base has
  already completed; `deferred wakeup -> publish … (d=22)` and
  `egdiffPredictions > 0` (mostly the committed-NVQ branch,
  `egdiffNVQBases > 0`).

Representative Layer 3 counts: `egdiffPredictions = 13929` (of which
`egdiffNVQBases = 12914`), `egdiffDeferredBindings = 437`,
`egdiffDeferredPredictions = 235`, `egdiffDeferredIgnored = 202`,
`numPredictionCorrect = 14154`, `numPredictionIncorrect = 0`; per-phase trace
attribution: 202 publishes at `d=2` (phase A), 33 at `d=22` (phase C).

The gate also runs a **negative control** (`EGDIFF_ON=0`) asserting an exact
checksum and that all `egdiff*` statistics are zero.

> **Not modelled (performance only):** the paper's 2-cycle deferral latency and
> the PRF-write / second-wakeup elision when a deferred prediction verifies. Any
> performance claim must account for this.

### Layer 2 criteria (implemented in `run_layer2.sh`)
- architectural checksum exact (a bad forward or bad wake corrupts it);
- `numPredictionIncorrect == 1` on the clean test;
- no value forwarded for `sn ∈ (M, M+1024]`, at least one forwarded after
  (`M` = mismatching load's seqNum), counting both normal and deferred paths;
- `egdiffLastMispSuppressed > 0`;
- `egdiffSquashed > 0` on the branchy test, with an exact checksum.

---

## 5. Deviations, design decisions, and "reviewer beware"

### 5.1 Hash functions: implementation deviates from the illustrative plan.md hash
The paper says only "hashed PC" and "different hash"; `plan.md` §2.1 gives
illustrative shift/XOR hashes. **Those illustrative hashes are wrong for
RISC-V**: with the C extension, two loads in the same 4-byte word differ only in
PC[1], and `((pc >> 2) ^ …)` / `((pc >> 4) ^ …)` discard that bit, so both loads
map to the same index *and* tag. An independent audit found this made one Layer 3
phase dead code (two adjacent loads ping-ponged a single entry and neither could
ever reach `ctr == 7`).

The implementation therefore uses, and `egdiff_sim.py` was updated in lock-step to
use:
```cpp
static unsigned indexHash(Addr pc) { return ((pc >> 1) ^ (pc >> 13)) & 0xFFFu; }
static uint16_t tagHash(Addr pc)   { return ((pc >> 1) ^ (pc >> 17) ^ (pc >> 29)) & 0x3FFFu; }
```
`plan.md` was deliberately left unmodified (per project instruction); a future
agent should update §2.1 to match. This matters beyond tests: any attack PoC whose
`L_atk` sits in the same 4-byte word as another load would silently never be
predicted.

### 5.2 Allocation probability vs. reproducibility
`egdiffColdDeterministicAlloc=True` makes a cold `u == 0` entry allocate with
probability 1.0. It is a **unit-test convenience only** and is **not** the
modelled hardware. Default is `False` (true 1/16). All results quoted above use
`False`. The PRNG is `std::mt19937_64` seeded by `egdiffSeed` (default 1) so runs
are reproducible.

### 5.3 Deferred wakeup must not "complete" the load
`InstructionQueue::wakeDependents()` also calls `memDepUnit[tid].completeInst()`
and `clearInIQ()/memOpDone(true)`. Calling that at deferred-prediction time would
falsely mark a *not-yet-executed* load as complete. Hence the new
`wakeRegDependents(DynInst*, bool isDeferred)` does only the
register/dependGraph/scoreboard part.

When `isDeferred` is true it additionally leaves all pinned-write accounting
(`decrNumPinnedWritesToComplete`, `setPinnedRegsWritten`) to the real writeback and
does not wake a pinned destination early; `IEW::lvpWakeDeferred` likewise skips the
PRF/scoreboard update for a pinned destination. The non-deferred path guards the
decrement with `> 0` so a deferred wakeup followed by the real writeback can never
underflow the counter to `-1`. (Note: the guard is a global change to upstream
`wakeRegDependents` behaviour, not EgDiff-specific, though it is a no-op for the
single-call case.)

### 5.8 A deferred wake is published only if the base itself verified
`EgDiffPredictor::completeLoad` only creates a `wakeTarget` when the base load's
own prediction verified (`res.ok`). If the base value-mispredicted, the younger
window is about to be squashed, so no wake is generated (previously a phantom
`deferred wakeup -> publish` event and a `egdiffDeferredPredictions` over-count
could occur).

### 5.4 Dispatch idempotence
IEW can re-process an instruction from its skid buffer, so `dispatchLoad` is
guarded: if a slot with the same `seqNum` already exists it returns "no
prediction" without allocating a second slot. Re-fetch after a squash gets a
**new** seqNum and allocates normally.

### 5.5 Faulting loads
`commitLoad` is skipped for `fault != NoFault` so a faulting load does not train
the table, while an `else` branch in `commit.cc` explicitly calls
`cpu->lvp->retireLoad(head_inst->seqNum)` so its speculative GVQ slot is popped
immediately at commit.

### 5.6 Table entry has no `valid` bit
`TableEntry` is the exact 88-bit layout (`tag,u,ctr,diff,distance`). A power-on
`tag == 0` entry cannot predict because `ctr == 0`. Allocation is gated by
`u == 0` + 1/16.

### 5.7 `predictLoad` and CVU
In EgDiff mode `predictLoad()` returns invalid (EgDiff uses `dispatchLoad`), and
`processStoreAddress()` is a no-op (EgDiff re-verifies every load at completion,
so store-invalidation is unnecessary). The FLOP path is untouched.

---

## 6. Additional hardware rule found in Layer 1 (self-stride lock-on)

Algorithm 1 locks onto the **first** distance whose difference is constant across
iterations. A value that is index-linear (`B[i] = 7*i + 41`) is constant at every
distance that is a multiple of loads-per-iteration, so the entry locks onto a
*self*-distance (observed: `d = 27`, `diff = 63`) instead of the intended `d = 2`.
The Layer 1 test therefore uses pseudorandom array contents so **only** `d = 2`
has a constant stride. This does not weaken the attack: the attacker controls
`L_atk`'s own value sequence; the victim's loads are only *read* from the GVQ.

---

## 7. Review resolutions & remaining Layer 3 item

1. **`lastMisp` accounting (RESOLVED — APPROVED):** `suppressUntil = seqNum + 1024`
   and `predictionSuppressed(sn) = sn <= suppressUntil` is exact to the single
   dynamic instruction because gem5 assigns strictly sequential `InstSeqNum`s at
   fetch.
2. **`retireFront` and `curIdx - d` indexing (RESOLVED — APPROVED):** popping
   `specGVQ.front()` inside `commitLoad()` at the exact instant the value is
   shifted into `nvq[0]` keeps `specGVQ[curIdx - d]` ($d \le \text{curIdx}$) and
   `nvq[d - curIdx - 1]` ($d > \text{curIdx}$) seamlessly contiguous.
3. **Faulting-load slot cleanup (RESOLVED — FIXED):** `commit.cc` now calls
   `cpu->lvp->retireLoad(head_inst->seqNum)` when `head_inst->fault != NoFault`.
4. **Deferred wake and `numPinnedWritesToComplete` (RESOLVED — FIXED):**
   `InstructionQueue::wakeRegDependents()` now guards
   `if (dest_reg->getNumPinnedWritesToComplete() > 0) dest_reg->decrNumPinnedWritesToComplete();`
   so calling `wakeRegDependents` at deferred-wakeup time followed by the actual
   load writeback in `IEW::writebackInsts` never underflows the counter to `-1`.
5. **`egdiffDeferredIgnored` stat wiring (RESOLVED — FIXED):**
   `EgDiffCompleteResult::wakeIgnored` is now set when a bound dependent has
   already completed (`dep.avail == true`) before its base load finishes, and
   increments `stats.egdiffDeferredIgnored`.
6. **Hash aliasing killed Layer 3 phase A (RESOLVED — FIXED):** independent audit
   (D1). Both hash functions now use `pc >> 1` (§5.1); `egdiff_sim.py` updated in
   lock-step. Phase A now publishes ~202 wakeups at `d=2`.
7. **Phantom deferred publish on a base misprediction (RESOLVED — FIXED):**
   audit D2, see §5.8.
8. **Deferred wake vs. register pinning (RESOLVED — FIXED):** audit D3, see §5.3.
9. **Instrumentation (RESOLVED — ADDED):** `egdiffNVQBases` (committed-NVQ base
   branch) and `egdiffWakeCleared` (bindings cleared on squash); the publish and
   ignored trace lines now carry the dependent PC (and publish its distance) so the
   gate can attribute events to phases.
10. **Layer 4 Mode 1 (RESOLVED — PASSES).** `poc_v1_specgvq_1shot.c` recovers the
    full 64-bit secret with EgDiff and leaks nothing with the predictor off or
    with the FLOP local LVP.
11. **Layer 4 Mode 2 (RESOLVED — PASSES).** After the review, the capture body,
    sentinel routing and verdict were fixed; `poc_v2_nvq_fence_bypass.c` now
    recovers the full 64-bit value with EgDiff and fails with the predictor off
    or with the FLOP local predictor.
12. **Layer 5** — SipHash-2-4 key extraction remains.

---

## 8. Reviewer checklist

- [ ] `scons build/RISCV/gem5.opt -j4` is clean.
- [ ] `run_layer1.sh` → `LAYER 1 PASSED` (17 checks).
- [ ] `run_layer2.sh` → `LAYER 2 PASSED` (10 checks).
- [ ] `run_layer3.sh` → `LAYER 3 PASSED` (13 checks, incl. phase attribution and the
      `EGDIFF_ON=0` negative control).
- [ ] `run_layer4.sh` → `LAYER 4 PASSED` (14 checks, both modes + isolation + controls).
- [ ] `run_layer5.sh` → `LAYER 5 PASSED` (24 checks, SipHash target + isolation + controls).
- [ ] `run_layer6.sh` → `LAYER 6 PASSED` (36 checks, Monocypher verify32 target + controls).
- [ ] With `EGDIFF_ON=0`, Layer 1/2 tests still produce exact checksums and zero
      EgDiff statistics (predictor must be inert).
- [ ] With `EGDIFF_TYPE=flop`, FLOP behaviour is unchanged (no EgDiff stats move).
- [ ] `git diff` touches only the files in §2.
- [ ] No `plan.md` edits are required by this implementation.
