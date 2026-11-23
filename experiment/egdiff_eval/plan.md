# EgDiff (ISCA 2026) in gem5: Test-Driven Implementation & Attack Evaluation Plan

**Branch:** `feat/egdiff-attack`  
**Target Simulator Binary:** `build/RISCV/gem5.opt` (and `build/RISCV/gem5.debug`)  
**Cross-Compiler for Test Binaries:** `riscv64-linux-gnu-gcc -static -O2`  
**Reference Behavioral Model (Auditor-Approved):** `experiment/egdiff_eval/egdiff_sim.py`  
**Reference Paper:** `docs/papers/Revisiting_Global_Value_Prediction_A_Resurgent_Complement_to_Local_Predictors.pdf` (ISCA 2026)  
**Prior Art Baseline Paper:** `docs/papers/deng2021new.pdf` (Deng & Szefer 2021)

---

## 1. Executive Summary & Architectural Goal

We are implementing **EgDiff** (an enhanced global load value predictor from ISCA 2026) inside gem5's `O3CPU` on a **single-core** setup, and demonstrating a novel microarchitectural attack family specific to EgDiff's global predictor architecture: **Cross-Load GVQ Secret Exfiltration ("GVQ Value Teleportation")**, in two modes:
1. **Mode 1 — In-Flight `SpecGVQ` Value Teleportation (1-Shot Victim):**
   - Attacker pre-trains their own load `L_atk` (`PC_atk != PC_victim`) offline to `ctr = 7`, `distance = d`, `diff = 0`.
   - Victim executes a branchless, transmitter-free function **once** that loads a 64-bit secret $S$ (`L_secret`), computes on it in registers, and **scrubs the architectural register (`s = 0`) before returning**.
   - Even though `s` was scrubbed, $S$ remains in the 32-entry Global Value Queue (`SpecGVQ` / circular GVQ). When `L_atk` dispatches $d \le 32$ loads later, EgDiff predicts $V_{\text{pred}}(L_{\text{atk}}) = \text{GVQ}[k-d] + 0 = S$ and writes $S$ **directly into the attacker's physical register** (either via Normal 3-cycle prediction or via +2-cycle **Deferred Prediction (`Wake` pointer binding)** when `L_secret` misses in L1D).
2. **Mode 2 — Post-Commit `NVQ` Capture & Replay Across Serializing Return Fences (`Algorithm 1`):**
   - Even if the victim executes a full pipeline-draining fence (`fence` / `lfence` / `dsb`) before returning—draining `SpecGVQ` completely—the committed secret $S$ moves into the **32-entry Non-Speculative Value Queue (`NVQ`)**, which is **never cleared on pipeline squashes or fences**.
   - At commit of `L_atk` (loading `0` at distance $d$ after `L_secret`), **Algorithm 1 (Distance Polling)** computes $\delta_{\text{new}} = 0 - \text{NVQ}[d] = -S$ and latches **`gdEntry.diff = -S`** into `Table[PC_atk]`.
   - Over ~42 calls (median 39, mean 41.8 under the stochastic 3-bit FPC counter), `gdEntry.ctr` saturates to `7` on `diff = -S`.
   - On the replay step (purely in attacker code, zero victim call), `L_atk` dispatches with a dummy base load of `0` at distance $d$ inside a **mispredicted branch shadow**, causing EgDiff to inject $0 + (-S) = -S$ into the attacker's register and leak all 8 bytes via Flush+Reload without ever committing a misprediction (so `ctr == 7` and `diff == -S` are never wiped).

---

## 2. Exact Hardware Specification of EgDiff (ISCA 2026)

Every structure and parameter MUST match `experiment/egdiff_eval/egdiff_sim.py` (which has been audited line-by-line against Sections III–V of the paper):

### 2.1 Prediction Table (`4096` Entries, 88 Bits/Entry = 44 KB)
- **Index Hash (12 bits):**
  ```cpp
  unsigned indexHash(Addr pc) const { return ((pc >> 2) ^ (pc >> 14)) & 0xFFFu; }
  ```
- **Tag Hash (14 bits, distinct hash function per Section III-A & V-A):**
  ```cpp
  uint16_t tagHash(Addr pc) const { return ((pc >> 4) ^ (pc >> 16) ^ (pc >> 28)) & 0x3FFFu; }
  ```
- **Entry Fields (`EgDiffTableEntry`):**
  - `uint16_t tag = 0;` (14-bit PC tag)
  - `uint8_t u = 0;` (2-bit usefulness counter, `0..3`, `MAXU = 3`)
  - `uint8_t ctr = 0;` (3-bit Forward Probabilistic Counter, `0..7`, `MAXCONF = 7`)
  - `uint64_t diff = 0;` (64-bit modular 2's complement stride $\delta_p$)
  - `uint8_t distance = 32;` (5-bit relative load distance $d \in [1, 32]$, `ORDER_N = 32`)
- **Forward Probabilistic Counter (FPC, Section III-A):**
  - Transition probability vector for `ctr` $= 0 \to 1, 1 \to 2, \dots, 6 \to 7$:
    $$P_{\text{FPC}} = \left\{1.0,\; \frac{1}{4},\; \frac{1}{4},\; \frac{1}{8},\; \frac{1}{8},\; \frac{1}{8},\; \frac{1}{8}\right\}$$
  - Use a deterministic seeded PRNG (e.g., `std::mt19937_64 rng(seed)`) so gem5 runs are 100% reproducible.
- **Allocation & Replacement Policy (Section III-A & III-B):**
  - At Commit, if `gdEntry.tag != tagHash(pc)`:
    - If `gdEntry.u == 0`, allocate/replace with probability **$\frac{1}{16}$** (or provide a SimObject param `coldDeterministicAlloc` defaulting to `false`, with exact $1/16$ probability when `u == 0`):
      ```cpp
      gdEntry.tag = t;
      gdEntry.u = 0;
      gdEntry.ctr = 0;
      gdEntry.distance = 32;
      gdEntry.diff = v_actual - nvq[32 - 1];
      ```
    - If `gdEntry.u > 0`, **skip allocation**.
- **10-Bit `TICK` Counter (Section III-B):**
  - Increments by 1 on every committed load update. Every `1024` committed loads, wraps to 0 and decrements `entry.u` by 1 for all entries with `entry.u > 0`.

### 2.2 Global `lastMisp` 1024-Instruction Suppression Window (Section III-A)
- Maintain `unsigned lastMispCountdown = 0;`.
- Whenever a predicted load mismatches during LQ verification (`predictedVal != actualVal`), set `lastMispCountdown = 1024`.
- Decrement `lastMispCountdown` as dynamic instructions dispatch (or track the CPU's dispatched `InstSeqNum` at the moment of misprediction: `InstSeqNum suppressUntilSeqNum = inst->seqNum + 1024;`, so any load with `inst->seqNum <= suppressUntilSeqNum` has predictions suppressed! Using `inst->seqNum <= suppressUntilSeqNum` in gem5 is exact to the single dynamic instruction without needing extra hooks on non-load instructions!).

### 2.3 Global Value Queue (`SpecGVQ[32]` + `NVQ[32]`, 141 Bits/Entry)
- **`SpecGVQ` (In-Flight Queue):**
  - `std::deque<SpecGVQEntry> specGVQ;` (up to 32 entries, matching the LQ; or sized to LQ entries).
  - Each `SpecGVQEntry` holds:
    - `InstSeqNum seqNum;`
    - `Addr pc;`
    - `uint64_t value = 0;`
    - `uint8_t distance = 0;`
    - `uint64_t diff = 0;`
    - `bool vp = false;` (valid prediction issued)
    - `bool misp = false;` (misprediction detected)
    - `bool avail = false;` (actual load execution completed in LQ)
    - `InstSeqNum wakeSeqNum = 0;` (0 = unbound; otherwise `seqNum` of the bound younger dependent load waiting for deferred wakeup)
    - `DynInstPtr inst = nullptr;` (pointer to the in-flight `DynInst` so deferred wakeup can write its destination register and wake its IQ dependents!)
- **`NVQ` (Committed Non-Speculative Queue):**
  - `std::array<uint64_t, 32> nvq{};`
  - `nvq[0]` corresponds to `NVQ[1]` in Algorithm 1 (the most recently committed load prior to the currently committing load); `nvq[31]` corresponds to `NVQ[32]`.

### 2.4 Commit-Stage Distance Polling (`Algorithm 1`, Page 6)
Executed at `Commit::commitHead` when a load commits with architectural value `v_actual`:
```cpp
if (gdEntry.tag == t) {
    uint8_t d = gdEntry.distance; // in [1..32]
    uint64_t delta_p = gdEntry.diff;
    uint64_t delta_a = v_actual - nvq[d - 1];

    if (delta_a != delta_p) {
        gdEntry.ctr = 0;
        gdEntry.u = 0;
        if (d > 1) {
            d = d - 1;
            gdEntry.distance = d;
            gdEntry.diff = v_actual - nvq[d - 1];
        } else {
            gdEntry.distance = 32;
            gdEntry.diff = v_actual - nvq[32 - 1];
        }
    } else {
        if (gdEntry.ctr < 7 && forwardProb(gdEntry.ctr)) {
            gdEntry.ctr++;
        }
        if (gdEntry.u < 3) {
            gdEntry.u++;
        }
    }
}
// Then shift v_actual into the front of NVQ:
for (int i = 31; i > 0; --i) nvq[i] = nvq[i - 1];
nvq[0] = v_actual;
```

---

## 3. Four Critical Microarchitectural & Exploit Rules (Discovered in Python Eval)

Any agent writing the C++ predictor and C test binaries MUST follow these four rules:

1. **Rule A (The `d_target + 1` Zero-Latching Rule in Algorithm 1):**
   - In Algorithm 1, `gdEntry.diff` is updated *during the mismatch at `distance = d_target + 1`* right after decrementing `d <- d - 1` (`gdEntry.diff = v_actual - NVQ[d_target]`).
   - Therefore, when stepping `L_atk`'s distance down to `d_target`, **`L_atk` must switch to loading `v_actual = 0` when `gdEntry.distance == d_target + 1`** (one step early!), so the transition `d_target + 1 -> d_target` latches `0 - S = -S` into `gdEntry.diff`.
2. **Rule B (The $d = 16$ Self-Stride Trap on Linear Stepping Sequences):**
   - Never use a linear sequence `v_k = k * C` to step `gdEntry.distance` down from `32` in a single-load loop! At $d = 16$, `NVQ[16]` holds `(k - 16) * C`, giving a constant difference `16 * C` that locks `gdEntry.distance` at $d = 16$ forever.
   - Always use a **non-linear sequence**: `uint64_t v_k = (k * k * k + k) * 0x9E3779B97F4A7C15ULL;`.
3. **Rule C (`SpecGVQ` + `NVQ` Base Value Resolution at Dispatch):**
   - When load $k$ dispatches at `cur_idx = specGVQ.size()` (before pushing load $k$) and hits in the table (`ctr == 7`, not suppressed by `lastMisp`) with `distance = d`:
     - If `cur_idx >= d`: the base load is still in-flight at `specGVQ[cur_idx - d]`.
       - If `base.avail || base.vp`: predict immediately (`V_pred = base.value + diff`).
       - Else (`!base.avail && !base.vp`): bind deferred wakeup (`if (base.wakeSeqNum == 0) base.wakeSeqNum = inst->seqNum;`).
     - If `cur_idx < d`: the base load at distance $d$ has already committed and sits at **`nvq[d - cur_idx - 1]`** (`avail == 1`). Predict immediately (`V_pred = nvq[d - cur_idx - 1] + diff`).
4. **Rule D (Gem5 `IEW::squashDueToMemOrder` Includes the Violating Load Itself):**
   - In `src/cpu/o3/iew.cc:517`, `squashDueToMemOrder(inst, tid)` sets `includeSquashInst[tid] = true`, which squashes and re-fetches `inst` itself.
   - When `inst` mispredicts at LQ verification, we set `suppressUntilSeqNum = inst->seqNum + 1024` and call `squashDueToMemOrder(inst, tid)`.
   - `ROB::squash(squash_num, tid)` pops all `specGVQ` entries with `seqNum > squash_num` (which includes the squashed `inst`).
   - When `inst` is immediately re-fetched on the correct path, `lastMisp` (`seqNum <= suppressUntilSeqNum`) is active, so the re-fetched `inst` is **not** predicted (`vp = false`), executes normally in the LQ, and commits at `Commit::commitHead`, where Algorithm 1 updates `gdEntry` (`ctr <- 0`, `d <- d - 1`, `diff <- delta_new`)!

---

## 4. Existing O3CPU Hook Locations in This Repo (Reference Map)

Do **not** break existing FLOP LVP functionality; add `predictorType = Param.String("flop", "Predictor type: 'flop' or 'egdiff'")` to `LoadValuePredictionUnit.py` so `--param='system.cpu[:].loadValuePredictor.predictorType="egdiff"'` (or a flag/config) selects EgDiff cleanly.

Here are the exact files and line numbers where LVP hooks already exist:
1. **`src/cpu/lvp/LoadValuePredictionUnit.py` & `src/cpu/lvp/SConscript`**
   - Defines `LoadValuePredictionUnit` SimObject and registers C++ sources and debug flags (`LVP`, `LVPAll`).
2. **`src/cpu/o3/dyn_inst.hh` (lines 223–231, 1223–1255)**
   - Stores `_lvpPredicted`, `_lvpPredictedVal`, `_lvpVerified` on `DynInst`.
3. **`src/cpu/o3/iew.cc` (lines 1024–1064)**
   - Dispatch-stage hook for loads (`if (inst->isLoad())`). Calls `cpu->lvp->predictLoad(...)`, writes `cpu->setReg(destReg, result.predictedValue, tid)`, and marks `scoreboard->setReg(destReg)`.
4. **`src/cpu/o3/inst_queue.cc` (lines 1533–1537)**
   - In `InstructionQueue::insert()`, checks `if (new_inst->lvpPredicted()) regScoreboard[dest_reg->flatIndex()] = true;`.
5. **`src/cpu/o3/lsq_unit.cc` (lines 1181–1265)**
   - In `LSQUnit::completeDataAccess()`, reads `actualVal = cpu->getReg(destReg, tid)`, verifies `inst->lvpPredicted()`, and calls `iewStage->squashDueToMemOrder(inst, tid)` on mismatch.
6. **`src/cpu/o3/commit.cc` (lines 1019–1022)**
   - In `Commit::commitInsts()`, right after `bool commit_success = commitHead(head_inst, num_committed);`, checks `if (head_inst->isLoad())`.
7. **`src/cpu/o3/rob.cc` (lines 454–471)**
   - In `ROB::squash(InstSeqNum squash_num, ThreadID tid)`, `squash_num` is the exact exclusive bound of surviving instructions (every instruction with `seqNum > squash_num` is being squashed).

---

## 5. Step-by-Step Test-Driven Implementation Layers (Layers 1 to 5)

**Golden Rule:** Do NOT move to Layer $N+1$ until Layer $N$ compiles with `scons build/RISCV/gem5.opt -j$(nproc)` and passes all semantic verification assertions of Layer $N$.

---

### Layer 1: Commit-Stage Core Engine (`EgDiffTable` + `NVQ` + `Algorithm 1` + `FPC` + `TICK`)

#### 1.1 Implementation Tasks
1. In `src/cpu/lvp/LoadValuePredictionUnit.py`, add configuration parameters to `LoadValuePredictionUnit`:
   - `predictorType = Param.String("flop", "LVP type: 'flop' or 'egdiff'")`
   - `egdiffTableEntries = Param.Unsigned(4096, "EgDiff prediction table entries")`
   - `egdiffOrder = Param.Unsigned(32, "EgDiff GVQ order n (max distance)")`
   - `egdiffColdDeterministicAlloc = Param.Bool(False, "Allocate cold u==0 entry with prob 1.0 instead of 1/16 for fast unit testing")`
2. In `src/cpu/lvp/load_value_prediction_unit.hh` & `.cc`:
   - Add the EgDiff data structures (`std::vector<EgDiffTableEntry> egdiffTable`, `std::array<uint64_t, 32> nvq`, `uint32_t tickCounter`, `std::mt19937_64 fpcRng`).
   - Implement `void commitLoad(ThreadID tid, InstSeqNum seqNum, Addr instPC, RegVal actualVal);` executing the 10-bit `TICK` counter decay, **Algorithm 1 (Distance Polling)**, $\frac{1}{16}$ tag-mismatch allocation when `u == 0`, and shifting `actualVal` into `nvq[0]`.
   - Add `DPRINTF(LVP, ...)` logs inside `commitLoad` showing `(PC, idx, tag, actualVal, old_d, new_d, old_diff, new_diff, ctr, u)`.
   - Add statistics to `LVPStats`:
     - `egdiffCommits`, `egdiffTableAllocations`, `egdiffPollingSteps`, `egdiffStrideMatches`, `egdiffFpcIncrements`, `egdiffTickDecays`.
3. In `src/cpu/o3/commit.cc` (around line 1022, inside `if (commit_success)`):
   - If `head_inst->isLoad() && head_inst->fault == NoFault && cpu->lvp && cpu->lvp->isEnabled()`:
     - Read `actualVal` from `head_inst->renamedDestIdx(0)` via `cpu->getReg(destReg, tid)` and call `cpu->lvp->commitLoad(tid, head_inst->seqNum, head_inst->pcState().instAddr(), actualVal);`.
   - In Layer 1, keep `predictLoad()` returning ` result.valid = false` when `predictorType == "egdiff"` so Layer 1 tests the commit-stage state machine in complete isolation.

#### 1.2 Layer 1 Verification Test (`tests/test-progs/egdiff_attack/test_layer1_polling.c`)
Write a small C program compiled with `riscv64-linux-gnu-gcc -static -O2` that executes a loop of 150 iterations containing two loads `L_base` and `L_target` (`L_target` value $= L_{\text{base}} + 40$) separated by 1 intermediate load ($d = 2$):
- **Run command:**
  ```bash
  build/RISCV/gem5.opt --debug-flags=LVP -d /tmp/opencode/l1_out \
    configs/deprecated/example/se.py --cpu-type=O3CPU --caches \
    --param='system.cpu[:].loadValuePredictor.predictorType="egdiff"' \
    --cmd=tests/test-progs/egdiff_attack/test_layer1_polling
  ```
- **Semantic Pass Criteria (Must all hold):**
  1. In `/tmp/opencode/l1_out/simout` (`--debug-flags=LVP`), `L_target`'s table entry allocates with `distance = 32` and steps `32 -> 31 -> ... -> 2` by `-1` on each mismatching commit.
  2. Upon reaching `distance = 2`, `gdEntry.diff` latches `40` (`0x28`), stops decrementing `distance`, and increments `ctr` probabilistically via FPC until `ctr == 7` and `u == 3`.
  3. At iteration 120, the test program changes `L_target`'s value to `L_base + 99`; verify in the log that on that single commit, `ctr` resets `7 -> 0`, `u` resets `3 -> 0`, and `distance` decrements `2 -> 1` (and on the next mismatch wraps `1 -> 32`).

---

### Layer 2: `SpecGVQ` Lifecycle + Normal 3-Cycle Dispatch Prediction + LQ Verification, `lastMisp` & Squash Rollback

#### 2.1 Implementation Tasks
1. Implement `SpecGVQ` (`std::deque<SpecGVQEntry> specGVQ`) in `LoadValuePredictionUnit`:
   - Add `LVPResult dispatchLoadEgDiff(const DynInstPtr &inst, ThreadID tid, Addr instPC);`:
     - Allocates a `SpecGVQEntry` for `inst` (`seqNum = inst->seqNum`, `pc = instPC`, `inst = inst`).
     - Checks `inst->seqNum > suppressUntilSeqNum`, `gdEntry.tag == tagHash(instPC)`, and `gdEntry.ctr == 7`.
     - Resolves base value at distance `d = gdEntry.distance` per **Rule C** (`cur_idx >= d` checks `specGVQ[cur_idx - d]`; `cur_idx < d` reads `nvq[d - cur_idx - 1]`).
     - If base value is available, computes `predVal = baseVal + gdEntry.diff`, sets `q_entry.vp = true`, `q_entry.value = predVal`, and returns `LVPResult{LVPClassification::Constant, predVal, true}`.
2. In `src/cpu/o3/iew.cc` (lines 1025–1063):
   - Call `predictLoad` / `dispatchLoadEgDiff` for every load at Dispatch. When `result.valid` is true, `inst->setLVPPrediction(...)`, `cpu->setReg(destReg, result.predictedValue, tid)`, and `scoreboard->setReg(destReg)` already forward the prediction into the PRF and mark the scoreboard ready!
3. In `src/cpu/o3/lsq_unit.cc` (`LSQUnit::completeDataAccess`, lines 1181–1265):
   - For `predictorType == "egdiff"`:
     - Call `bool correct = cpu->lvp->completeLoadEgDiff(inst, tid, actualVal);` on **every** non-faulting load completion.
     - Inside `completeLoadEgDiff`:
       - Locate `q_entry` in `specGVQ` by `inst->seqNum`.
       - Set `q_entry.avail = true` and `q_entry.value = actualVal`.
       - If `q_entry.vp`:
         - If `predictedVal == actualVal`: increment `stats.numPredictionCorrect`, return `true`.
         - If `predictedVal != actualVal`: set `q_entry.misp = true`, set `suppressUntilSeqNum = inst->seqNum + 1024`, increment `stats.numPredictionIncorrect`, and return `false` (caller then invokes `iewStage->squashDueToMemOrder(inst, tid)`).
4. In `src/cpu/o3/rob.cc` (`ROB::squash(InstSeqNum squash_num, ThreadID tid)`, line 471):
   - Add:
     ```cpp
     if (cpu->lvp && cpu->lvp->isEnabled()) {
         cpu->lvp->squashFrom(squash_num, tid);
     }
     ```
   - `squashFrom(squash_num, tid)` pops all entries from the back of `specGVQ` with `seqNum > squash_num` (and clears any `wakeSeqNum > squash_num` on surviving older entries).
   - In `commitLoad()`, pop the committed load from the front of `specGVQ` (`if (!specGVQ.empty() && specGVQ.front().seqNum <= seqNum) specGVQ.pop_front();`).

#### 2.2 Layer 2 Verification Test (`tests/test-progs/egdiff_attack/test_layer2_spec_and_squash.c`)
- **Semantic Pass Criteria (Must all hold):**
  1. **Accurate Prediction & Dependent Wakeup:** In a global-stride loop ($I_2 = I_1 + 40$), once `ctr == 7`, `stats.txt` shows `numPredictions > 50` and `numPredictionIncorrect == 0`.
  2. **Misprediction Squash & 1024-Instruction `lastMisp` Blackout:** When the test alters the stride once after `ctr == 7`, `numPredictionIncorrect` increments by `1`, `IEW::squashDueToMemOrder` fires, and **zero** predictions are issued for the next 1,024 instructions (`egdiffLastMispSuppressed > 0`), after which predictions resume automatically.
  3. **Branch-Shadow Non-Destructive Squash Isolation:** The test executes a mispredicted branch whose wrong path performs a load with a wrong value (whose address depends on a slow division so the branch resolves first). Verify in `--debug-flags=LVP` that `ROB::squash` purges the wrong-path load from `SpecGVQ` and that `gdEntry.ctr` remains `7` and `gdEntry.distance` is NOT decremented!

---

### Layer 3: Deferred Prediction via `Wake` Pointer Binding & Asynchronous IQ Wakeup (Section III-C)

#### 3.1 Implementation Tasks
1. **Binding at Dispatch (`dispatchLoadEgDiff`):**
   - When `cur_idx >= d` and `base_entry = specGVQ[cur_idx - d]` has `!base_entry.avail && !base_entry.vp`:
     - Set `q_entry.vp = false`, `q_entry.distance = d`, `q_entry.diff = diff`.
     - One-to-one binding: `if (base_entry.wakeSeqNum == 0) { base_entry.wakeSeqNum = inst->seqNum; ++stats.egdiffDeferredBindings; }`.
2. **Deferred Wakeup at Base Load Completion (`completeLoadEgDiff` & `src/cpu/o3/lsq_unit.cc`):**
   - In `completeLoadEgDiff(inst, tid, actualVal)`:
     - When `q_entry` (at index `base_idx` in `specGVQ`) completes (`q_entry.avail = true`) and `q_entry.wakeSeqNum != 0`:
     - Search `specGVQ` for `dep_idx` with `seqNum == q_entry.wakeSeqNum`.
     - If found, check:
       1. `!dep_entry.avail && !dep_entry.vp` (if `dep_entry.avail` is already true because `dep_entry` completed before `base_entry`, increment `stats.egdiffDeferredIgnored` and do nothing per Section III-C!),
       2. `(dep_idx - base_idx) == dep_entry.distance` (relative distance still valid), and
       3. `dep_entry.seqNum > suppressUntilSeqNum` (`lastMisp` inactive).
     - If all 3 hold:
       - Compute `uint64_t deferredPred = actualVal + dep_entry.diff;`.
       - Set `dep_entry.value = deferredPred`, `dep_entry.vp = true`, and call `dep_entry.inst->setLVPPrediction(LVPClassification::Constant, deferredPred)`.
       - Return `dep_entry.inst` (and `deferredPred`) to `LSQUnit::completeDataAccess` so `LSQUnit` writes `deferredPred` to `dep_inst`'s physical destination register (`cpu->setReg(depDestReg, deferredPred, tid)`), marks the scoreboard (`iewStage->scoreboard->setReg(depDestReg)`), and wakes up instructions waiting on `dep_inst` in the Issue Queue (`iewStage->instQueue.wakeDependents(dep_inst)`)!
   - **Important O3CPU Detail for `instQueue.wakeDependents(dep_inst)`:**
     - When `dep_inst` later completes its own actual memory access in `LSQUnit::completeDataAccess` (`inst->lvpPredicted()` is now `true`), check what `LSQUnit::writebackInst` / `iewStage->instQueue.wakeDependents(inst)` does if `wakeDependents(dep_inst)` was already called during deferred wakeup (or check how normal Dispatch LVP predictions handle `writebackInst`!).
     - Let's note how normal Dispatch LVP works in this repo:
       - In `src/cpu/o3/inst_queue.cc:1533`, for a load predicted at Dispatch (`new_inst->lvpPredicted() == true`), `InstructionQueue::insert()` simply sets `regScoreboard[dest_reg->flatIndex()] = true;` without adding younger dependent instructions to `dependGraph[dest_reg]` (or when `new_inst` later finishes in `IEW::writebackInsts`, `instQueue.wakeDependents(inst)` sees an empty `dependGraph` list for `dest_reg`!).
       - For a **Deferred Prediction**, `dep_inst` had `lvpPredicted() == false` when it was inserted into the `InstructionQueue` at Dispatch, so younger instructions that depend on `dep_inst` WERE added to `dependGraph` waiting on `dep_inst`'s destination register!
       - Therefore, when `instQueue.wakeDependents(dep_inst)` is called at Deferred Wakeup time, `wakeDependents` walks and clears `dependGraph[dep_dest_reg]` and marks `regScoreboard[dep_dest_reg] = true`! Then, when `dep_inst` later completes its actual load in `IEW::writebackInsts` and calls `wakeDependents(dep_inst)` a second time, verify that `dependGraph.empty(dep_dest_reg)` is already true so the second call is a safe no-op (check `InstructionQueue::wakeDependents` in `src/cpu/o3/inst_queue.cc` to confirm!).

#### 3.2 Layer 3 Verification Test (`tests/test-progs/egdiff_attack/test_layer3_deferred_wake.c`)
- **Semantic Pass Criteria (Must all hold):**
  1. When `L_base` misses in L1D (via `flush_all_l1()`) and `L_dep` dispatches right after it (with `L_dep` also missing in L1D), `stats.txt` records `egdiffDeferredBindings > 0`, `egdiffDeferredPredictions > 0`, and `numPredictionCorrect > 0`.
  2. When `L_dep` hits in L1D (3 cycles) while `L_base` misses in L1D (12+ cycles), `L_dep` completes first (`avail = 1`), and when `L_base` completes later, `stats.txt` records `egdiffDeferredIgnored > 0` with zero crashes or state corruption.

---

### Layer 4: End-to-End Attack PoC Binaries in gem5

Create `tests/test-progs/egdiff_attack/` with two standalone C attack programs compiled with `riscv64-linux-gnu-gcc -static -O2`:

#### 4.1 `poc_v1_specgvq_1shot.c` (Mode 1: 1-Shot In-Flight `SpecGVQ` Value Teleportation)
- **Victim Function (`__attribute__((noinline))`):**
  ```c
  __attribute__((noinline))
  uint64_t victim_check_key(const uint64_t *secret_ptr, uint64_t user_token) {
      uint64_t s = *secret_ptr;          // L_secret
      uint64_t ok = (s == user_token);   // Branchless compare
      asm volatile("" : "+r"(s));
      s = 0;                             // Explicit register scrub before return!
      return ok;
  }
  ```
- **Attack Sequence (per secret byte $b \in [0..7]$):**
  1. **Offline Self-Training (~80 iterations in attacker code):**
     - Execute `d` dummy loads of `0` followed by `L_atk`.
     - Obey **Rule A** (`v_atk = 0` when `distance in {d+1, d}`) and **Rule B** (non-linear sequence `v_k = (k*k*k + k) * 0x9E3779B97F4A7C15ULL` when stepping `distance` down from `32` to `d+1`).
     - Wait 1,024 instructions after the last stepping mismatch so `lastMisp` is `0` while `ctr` saturates to `7` (`diff = 0`, `distance = d`).
  2. **Evict `probe_area[256 * 64]` and `atk_slow_addr` from L1D.**
  3. **Call `victim_check_key(&secret, 0)` ONCE.**
  4. **Immediately execute `L_atk` (reading `0` from `atk_slow_addr`, whose address depends on a multi-cycle `div` chain so `L_atk` stays unverified in the LQ for 30+ cycles):**
     - EgDiff predicts `V_pred(L_atk) = GVQ[k - d] + 0 = secret` and forwards `secret` into `L_atk`'s destination register!
     - Dependent transient transmitter executes `probe_area[((val >> (8 * byte_idx)) & 0xFF) * 64]`.
  5. **Reload `probe_area[0..255]` using `rdcycle()` (`csrrs %0, cycle, x0`)** and record the winning byte.

#### 4.2 `poc_v2_nvq_fence_bypass.c` (Mode 2: Post-Commit `NVQ` Capture & Replay Across Return Fence)
- **Hardened Victim Function:** Same as above, PLUS a full hardware fence (`asm volatile("fence rw, rw" ::: "memory");`) right before `return` so the pipeline/LQ drains before returning to the caller.
- **Attack Sequence:**
  1. **Pre-step `Table[PC_atk].distance` to `d + 1`** offline using the non-linear sequence (**Rule B**).
  2. **Call the fenced victim 65 times**, executing `L_atk` (loading `0`) immediately after each return:
     - Call 1 steps `distance` from `d + 1 -> d` and latches `gdEntry.diff = 0 - S = -S` (**Rule A**).
     - Calls 2..65 saturate `gdEntry.ctr` to `7` on `diff = -S`.
  3. **Non-Destructive Branch-Shadow Replay (8 iterations for bytes `0..7`, zero victim calls):**
     - Train a branch predictor condition `if (cond)` to be taken; on the replay iteration, pass `cond = 0` (delayed by a `div` chain) with a dummy load of `0` at distance `d` before `L_atk` (whose address is also delayed past branch resolution).
     - Inside the transient branch shadow, `L_atk` receives `V_pred = 0 + gdEntry.diff = -S`, negates it (`S = -(-S)`), and indexes `probe_area[((S >> (8 * byte_idx)) & 0xFF) * 64]`.
     - The outer branch resolves and squashes `L_atk` **before `L_atk` verifies in the LQ or commits**, so `gdEntry.ctr` stays `7` and `gdEntry.diff` stays `-S` across all 8 byte extractions!

#### 4.3 Layer 4 Verification Gate
- **Must Verify:**
  1. Both `poc_v1_specgvq_1shot` and `poc_v2_nvq_fence_bypass` recover **8 / 8 bytes (`100%`)** of the 64-bit secret in gem5 when `predictorType = "egdiff"`.
  2. Running the exact same binaries with `predictorType = "flop"` (Apple constant LVP) or `lvp.enabled = False` recovers **0 / 8 bytes (`NO-LEAK`)**.

---

### Layer 5: Real-World Cryptographic Target (**SipHash-2-4** Key Extraction)

#### 5.1 Implementation Tasks
- Create `tests/test-progs/egdiff_attack/siphash_attack.c` containing the unmodified reference C implementation of **SipHash-2-4** (`uint64_t siphash24(const void *src, unsigned long src_sz, const uint8_t key[16])`).
- Notice that `siphash24` loads the two 64-bit secret key words (`k0` and `k1`), performs pure register-only ARX (`+`, `^`, bit-rotate) rounds with **zero secret-dependent branches** and **zero secret-dependent memory accesses**, and returns the 64-bit tag.
- Use `objdump -d` on the compiled `siphash_attack` RISC-V binary to read the exact static load count $d_{k0}$ and $d_{k1}$ from `k0`/`k1` to the caller's `L_atk` (typically $d \in [1, 3]$ at `-O2`).
- Run Mode 2 (`NVQ` Capture & Replay) or Mode 1 (`SpecGVQ` Teleportation) for $d = d_{k0}$ and $d = d_{k1}$ to extract the entire 128-bit (`16-byte`) secret SipHash key (`k0`, `k1`) via Flush+Reload!

#### 5.2 Layer 5 Verification Gate
- **Must Verify:**
  - `siphash_attack` running on `build/RISCV/gem5.opt` with `predictorType = "egdiff"` prints and verifies **16 / 16 bytes (`100%`)** of the 128-bit SipHash secret key (`k0`, `k1`), while leaking **0 / 16 bytes** when EgDiff is disabled.

---

## 6. Quick-Reference Commands for the Implementing Agent

```bash
# 1. Re-run the auditor-approved Python behavioral simulator & C exit-tail profiler:
python3 experiment/egdiff_eval/egdiff_sim.py
python3 experiment/egdiff_eval/c_exit_tail_eval.py

# 2. Build gem5 (RISC-V target):
scons build/RISCV/gem5.opt -j$(nproc)

# 3. Compile a RISC-V test binary for gem5 SE mode:
riscv64-linux-gnu-gcc -static -O2 -o tests/test-progs/egdiff_attack/<name> \
    tests/test-progs/egdiff_attack/<name>.c

# 4. Run gem5 with EgDiff and LVP debug tracing:
build/RISCV/gem5.opt --debug-flags=LVP -d /tmp/opencode/egdiff_run \
    configs/deprecated/example/se.py --cpu-type=O3CPU --caches \
    --param='system.cpu[:].loadValuePredictor.predictorType="egdiff"' \
    --cmd=tests/test-progs/egdiff_attack/<name>
```
