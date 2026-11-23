# EgDiff Behavioral Security Evaluation (`experiment/egdiff_eval/`)

Behavioral model and Monte Carlo evaluation of **EgDiff** (*"Revisiting Global Value Prediction: A Resurgent Complement to Local Predictors"*, ISCA 2026) implementing the exact hardware parameters from Sections III–V:
- **Prediction Table:** 4K entries, 88 bits/entry (`14b tag`, `2b u`, `3b FPC ctr` with `{1, 1/4, 1/4, 1/8, 1/8, 1/8, 1/8}`, `64b diff`, `5b distance` $\in [1, 32]$).
- **Global Value Queue (64 entries):** 32-entry `SpecGVQ` (with `Avail` and `Wake` deferred-prediction binding) + 32-entry committed `NVQ`.
- **Commit-Time Update:** Exact **Algorithm 1 (Distance Polling)** + 10-bit `TICK` counter + 1024-instruction global `lastMisp` blackout.

## Run
```bash
python3 experiment/egdiff_eval/egdiff_sim.py
python3 experiment/egdiff_eval/c_exit_tail_eval.py
```

## gem5 implementation

The gem5 port lives on branch `feat/egdiff-attack`; the layered, test-driven
plan and current status are in [`plan.md`](plan.md). Layer 1 (commit-stage
engine: Algorithm 1, FPC, TICK, NVQ) is verified by
`tests/test-progs/egdiff_attack/run_layer1.sh`.

## Independent Subagent Audit against ISCA 2026 Paper
An independent auditor subagent verified `egdiff_sim.py` line-by-line against `Revisiting_Global_Value_Prediction_A_Resurgent_Complement_to_Local_Predictors.pdf` and approved:
- Exact 88-bit `TableEntry` structure (`14b tag`, `2b u`, `3b ctr`, `64b diff`, `5b distance`) with no software-only valid bit.
- Exact $1/16$ allocation probability on tag mismatch when `u == 0` and 10-bit `TICK` (`1024` commits) decay of `u`.
- Automatic 1024-instruction `lastMisp` countdown stepping on load dispatch.
- Full 64-entry GVQ support (`SpecGVQ` in-flight lookup + deferred `Wake`/`Avail` binding + circular-buffer/committed `NVQ` lookup when `cur_idx < d`, including $d = 32$) and clearing of invalidated `wake` pointers on branch/value squashes.

## Compiled C Helper Exit-Tail Load Count Profile (`c_exit_tail_eval.py`)
Compiled 7 security/crypto/sandbox/data-structure C helper functions across 5 GCC optimization levels (`-O0`, `-O1`, `-O2`, `-O3`, `-Os`, 35 binary variants total) with `-fno-omit-frame-pointer -fstack-protector-strong`:
- **35 / 35 (100.0%)** stay well inside EgDiff's 32-load GVQ horizon (`d <= 32`).
- **Observed GVQ distance `d` from `L_secret` to caller's `L_atk`:** $d \in [1, 7]$ loads (even when clobbering all 6 callee-saved registers and checking a stack canary in `helper7_full_spill_and_canary`, $d = 7 \ll 32$).
- **Cross-call load jitter after `L_secret`:** `0` loads (function epilogue paths after the secret load are branchless).

## Key Experimental Findings & Hardware Rules Discovered

### 1. Two Non-Obvious Hardware Rules Required for Reliable Exploitation
Running the behavioral model exposed two critical properties of **Algorithm 1** that must be obeyed by any C/assembly PoC in gem5:
1. **The `d_target + 1` Zero-Latching Rule:**
   In Algorithm 1 (lines 6–10), `gdEntry.diff` is updated *during the mismatch at `distance = d_target + 1`* right after decrementing `d <- d - 1` (`gdEntry.diff <- v_actual - NVQ[d_target]`). Therefore, the attacker's load `L_atk` must switch from its stepping sequence to loading `v_actual = 0` when **`gdEntry.distance == d_target + 1`** (not waiting until `distance == d_target`), so that the transition `d_target + 1 -> d_target` latches `0 - S = -S` into `gdEntry.diff`.
2. **The $d = 16$ Self-Stride Trap on Linear Stepping Sequences:**
   When stepping `gdEntry.distance` down from `32` in a single-load offline loop, a linear sequence $v_k = k \cdot C$ fails at $d = 16$ because $\text{NVQ}[16] = (k - 16) \cdot C$, yielding a constant stride $\delta_a = 16 \cdot C$ that causes Algorithm 1 to lock onto $d = 16$ forever. Using a **non-linear sequence** (e.g., $v_k = (k^3 + k) \cdot C$) eliminates self-stride correlation across all $d \in [1, 32]$.

### 2. Summary of Results (`egdiff_sim.py`)
- **Experiment 1 (Version 1 — 1-Shot In-Flight `SpecGVQ` Value Teleportation):**
  - **100% 1-shot recovery** of the 64-bit secret (`0xdeadcafebeef1337`) in both **Mode 1A (Normal 3-cycle prediction under ROB-head stall)** and **Mode 1B (Deferred `Wake` pointer prediction on victim L2 miss)** after 54 offline attacker training commits and **1 single victim execution**.
- **Experiment 2 (Version 2 — Post-Commit `NVQ` Capture & Replay Across Serializing Fences):**
  - **500/500 (100.0%)** full 64-bit secret recovery even when the victim drains the entire pipeline (`SpecGVQ` empty) before returning.
  - Victim calls required to capture `-S` and saturate the 3-bit FPC (`ctr = 7`): **mean = 41.9, median = 40, 90th percentile = 64, 99th percentile = 83**.
  - **Branch-Shadow Non-Destructive Replay:** Squashing the replay load via an older branch before Commit allows leaking all 8 bytes of the 64-bit secret while keeping `gdEntry.ctr == 7` and `gdEntry.diff == -S` intact.
- **Experiment 3 (Constant-Value "Decoy Density" Test):**
  - A naive attacker (holding `v_actual = 0` fixed while polling from 32) drops to **0.0% success** as soon as there is $\ge 1$ constant decoy at $d > d_{\text{target}}$.
  - Our attacker (pre-stepping `distance` or modulating `v_actual` non-linearly until `distance == d_target + 1`) achieves **98%–100% success even when 31/31 slots (100% decoy density)** in the 32-load window are constant decoys.
- **Experiment 4 (Dynamic Load-Count Jitter Sensitivity):**
  - Quantifies the boundary between Version 2 (`NVQ` multi-call capture, which achieves 100% at 0% jitter and 78% at 2% per-call load jitter) and Version 1 (`SpecGVQ` 1-shot teleportation, which tolerates arbitrary inter-call load jitter because it only requires a single victim call).
