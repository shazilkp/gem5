#!/usr/bin/env python3
"""
Cycle- and Algorithm-Faithful Behavioral Simulator of EgDiff (ISCA 2026)
Paper: "Revisiting Global Value Prediction: A Resurgent Complement to Local Predictors"

Models:
  1. 4K-entry Prediction Table (88 bits/entry: 14b tag, 2b u, 3b FPC ctr, 64b diff, 5b distance)
  2. 3-bit Forward Probabilistic Counter (FPC) with vector {1, 1/4, 1/4, 1/8, 1/8, 1/8, 1/8}
  3. 10-bit TICK counter decaying u-bits every 1024 commits + 1/16 allocation probability when u == 0
  4. Global `lastMisp` 1024-instruction suppression window
  5. 64-entry Global Value Queue (GVQ):
     - 32-entry Speculative GVQ (lockstep with 32-entry Load Queue, with `Wake` & `Avail` deferred prediction)
     - 32-entry Non-Speculative GVQ (NVQ, committed loads FIFO)
  6. Exact Algorithm 1 (Distance Polling Algorithm) at Commit stage
"""

import random
import statistics
from dataclasses import dataclass
from typing import List, Optional, Tuple

MASK64 = 0xFFFFFFFFFFFFFFFF
FPC_PROBS = [1.0, 0.25, 0.25, 0.125, 0.125, 0.125, 0.125]
MAXCONF = 7
MAXU = 3
ORDER_N = 32
TABLE_SIZE = 4096
LAST_MISP_WINDOW = 1024
TICK_PERIOD = 1024


def index_hash(pc: int) -> int:
    """12-bit direct index hash for the 4K-entry table.

    Uses pc >> 1 (not pc >> 2): RISC-V with the C extension has 2-byte-aligned
    instructions, and two loads in the same 4-byte word must map to distinct
    entries.  The illustrative pc >> 2 index in plan.md aliases them.
    """
    return ((pc >> 1) ^ (pc >> 13)) & 0xFFF


def tag_hash(pc: int) -> int:
    """14-bit tag hash, a different function than index_hash and sensitive to
    PC[1] for the same reason."""
    return ((pc >> 1) ^ (pc >> 17) ^ (pc >> 29)) & 0x3FFF


@dataclass
class TableEntry:
    """Exact 88-bit Prediction Table Entry (Section V-D, page 11):
    14-bit tag + 2-bit u + 3-bit FPC ctr + 64-bit diff + 5-bit distance = 88 bits."""
    tag: int = 0          # 14-bit tag
    u: int = 0            # 2-bit usefulness counter (0..3)
    ctr: int = 0          # 3-bit FPC counter (0..7)
    diff: int = 0         # 64-bit stride (unsigned 2's complement)
    distance: int = 32    # 5-bit distance in [1..32]


@dataclass
class SpecGVQEntry:
    """141-bit Speculative GVQ Entry (Table I, page 7)."""
    seq_num: int = 0
    pc: int = 0
    value: Optional[int] = None   # 64-bit value
    distance: int = 0             # 5-bit distance
    diff: int = 0                 # 64-bit diff
    vp: int = 0                   # 1-bit valid prediction flag
    misp: int = 0                 # 1-bit misprediction flag
    avail: int = 0                # 1-bit base value readiness / LQ completed
    wake: Optional[int] = None    # 5-bit bound wake-up entry index (None = unbound)
    prf_dest_val: Optional[int] = None  # Value written into destination PRF


class EgDiffPredictor:
    def __init__(self, seed: int = 42):
        self.rng = random.Random(seed)
        self.table: List[TableEntry] = [TableEntry() for _ in range(TABLE_SIZE)]
        # Speculative GVQ: circular buffer of up to 32 in-flight loads (lockstep with LQ)
        self.spec_gvq: List[SpecGVQEntry] = []
        # Non-Speculative GVQ (NVQ): last 32 committed load values (index 1 = newest, 32 = oldest)
        self.nvq: List[int] = [0] * ORDER_N
        self.last_misp_countdown: int = 0
        self.tick_counter: int = 0
        self.seq_counter: int = 0

    def step_instructions(self, num_insts: int = 1):
        """Advance dynamic instruction counter for the 1024-inst lastMisp window."""
        if self.last_misp_countdown > 0:
            self.last_misp_countdown = max(0, self.last_misp_countdown - num_insts)

    def _forward_prob(self, ctr: int) -> bool:
        return self.rng.random() < FPC_PROBS[ctr]

    def dispatch_load(self, pc: int, insts_since_last_load: int = 4) -> SpecGVQEntry:
        """
        Dispatch stage (Section III-B & III-C):
        Allocates a Speculative GVQ entry for every candidate load.
        If tag matches, ctr == MAXCONF (7), and lastMisp == 0:
          - If base value at distance `d` is in-flight in SpecGVQ (cur_idx >= d):
            * If available (avail == 1 or vp == 1), computes V_pred = V_base + diff (Normal 3c prediction).
            * If unavailable (avail == 0 and vp == 0), binds `wake` on base entry (Deferred prediction).
          - If base load at distance `d` has already committed (cur_idx < d):
            * Its value resides in the circular buffer / NVQ at index `d - cur_idx - 1` (avail == 1),
              so computes V_pred = V_base + diff (Normal 3c prediction).
        """
        assert len(self.spec_gvq) < ORDER_N, "LQ / SpecGVQ full (32 entries)"
        self.step_instructions(insts_since_last_load)
        self.seq_counter += 1
        cur_idx = len(self.spec_gvq)
        q_entry = SpecGVQEntry(seq_num=self.seq_counter, pc=pc)

        idx = index_hash(pc)
        t = tag_hash(pc)
        gd_entry = self.table[idx]

        if (gd_entry.tag == t and
                gd_entry.ctr == MAXCONF and self.last_misp_countdown == 0):
            d = gd_entry.distance
            diff = gd_entry.diff
            q_entry.distance = d
            q_entry.diff = diff

            if cur_idx >= d:
                # Base entry is `d` positions earlier among currently in-flight SpecGVQ entries
                base_idx = cur_idx - d
                base_entry = self.spec_gvq[base_idx]
                if base_entry.avail == 1 or base_entry.vp == 1:
                    # Normal prediction: base value is ready
                    pred_val = (base_entry.value + diff) & MASK64
                    q_entry.value = pred_val
                    q_entry.vp = 1
                    q_entry.prf_dest_val = pred_val
                else:
                    # Deferred prediction (Section III-C): bind wake pointer (one-to-one)
                    q_entry.vp = 0
                    if base_entry.wake is None:
                        base_entry.wake = cur_idx
            else:
                # Base load at distance `d` has already committed (avail == 1 in circular buffer / NVQ)
                nvq_idx = d - cur_idx - 1
                base_val = self.nvq[nvq_idx]
                pred_val = (base_val + diff) & MASK64
                q_entry.value = pred_val
                q_entry.vp = 1
                q_entry.prf_dest_val = pred_val

        self.spec_gvq.append(q_entry)
        return q_entry

    def execute_load_in_lq(self, q_entry: SpecGVQEntry, actual_val: int) -> bool:
        """
        LQ Execution & Verification stage (Section III-B & III-C):
        1. If q_entry held a valid prediction (vp == 1), verifies predicted vs. actual_val.
           On mismatch: sets misp = 1, triggers lastMisp (1024 insts), and squashes younger SpecGVQ entries.
        2. Records actual_val into q_entry.value and sets avail = 1.
        3. If q_entry has a bound `wake` dependent entry that has not completed yet (avail == 0),
           triggers Deferred Prediction (+2 cycles) and writes V_pred into dependent's PRF!
        Returns True if verification passed (or no prediction), False if value misprediction squash occurred.
        """
        actual_val &= MASK64
        cur_idx = self.spec_gvq.index(q_entry)

        if q_entry.vp == 1:
            if q_entry.value != actual_val:
                q_entry.misp = 1
                q_entry.value = actual_val
                q_entry.avail = 1
                q_entry.prf_dest_val = actual_val
                # Trigger global lastMisp suppression for 1024 instructions
                self.last_misp_countdown = LAST_MISP_WINDOW
                # Squash all younger speculative entries in SpecGVQ and clear dangling wake pointers
                self.spec_gvq = self.spec_gvq[:cur_idx + 1]
                for e in self.spec_gvq:
                    if e.wake is not None and e.wake >= len(self.spec_gvq):
                        e.wake = None
                return False
            else:
                # Prediction matched! Per Section III-D (Fig. 7c), skip PRF writeback
                q_entry.avail = 1
        else:
            q_entry.value = actual_val
            q_entry.avail = 1
            q_entry.prf_dest_val = actual_val

        # Check for Deferred Prediction wakeup (Section III-C)
        if q_entry.wake is not None:
            dep_idx = q_entry.wake
            if dep_idx < len(self.spec_gvq):
                dep_entry = self.spec_gvq[dep_idx]
                # Verify dependent has not already completed (avail == 0), has not already predicted (vp == 0),
                # and distance still matches relative position
                if (dep_entry.avail == 0 and dep_entry.vp == 0 and
                        (dep_idx - cur_idx) == dep_entry.distance):
                    if self.last_misp_countdown == 0:
                        pred_val = (actual_val + dep_entry.diff) & MASK64
                        dep_entry.value = pred_val
                        dep_entry.vp = 1
                        dep_entry.prf_dest_val = pred_val

        return True

    def branch_squash_after(self, older_seq_num: int):
        """
        Squash all speculative loads younger than `older_seq_num` (e.g., on branch misprediction).
        Per Section III & III-A: only SpecGVQ is rolled back; NVQ and Prediction Table remain intact.
        """
        self.spec_gvq = [e for e in self.spec_gvq if e.seq_num <= older_seq_num]
        for e in self.spec_gvq:
            if e.wake is not None and e.wake >= len(self.spec_gvq):
                e.wake = None

    def commit_oldest_load(self, deterministic_alloc_on_cold: bool = False) -> SpecGVQEntry:
        """
        Commit stage (Section III-A & III-B, Algorithm 1):
        Pops the oldest load from SpecGVQ, updates the Prediction Table using NVQ via Algorithm 1,
        and pushes the committed actual value into the NVQ FIFO.
        """
        assert len(self.spec_gvq) > 0 and self.spec_gvq[0].avail == 1
        q_entry = self.spec_gvq.pop(0)
        # Adjust any remaining wake indices in SpecGVQ since head popped
        for e in self.spec_gvq:
            if e.wake is not None:
                e.wake = e.wake - 1 if e.wake > 0 else None

        v_actual = q_entry.value & MASK64
        pc = q_entry.pc
        idx = index_hash(pc)
        t = tag_hash(pc)
        gd_entry = self.table[idx]

        # Periodic 10-bit TICK counter decay of useful bits (Section III-B)
        self.tick_counter += 1
        if self.tick_counter >= TICK_PERIOD:
            self.tick_counter = 0
            for entry in self.table:
                if entry.u > 0:
                    entry.u -= 1

        if gd_entry.tag == t:
            # --- Algorithm 1: Distance Polling Algorithm (page 6) ---
            d = gd_entry.distance
            delta_p = gd_entry.diff
            # NVQ is 1-indexed in Algorithm 1: NVQ[1] is most recent committed load
            nvq_d_val = self.nvq[d - 1]
            delta_a = (v_actual - nvq_d_val) & MASK64

            if delta_a != delta_p:
                gd_entry.ctr = 0
                gd_entry.u = 0
                if d > 1:
                    d = d - 1
                    gd_entry.distance = d
                    delta_new = (v_actual - self.nvq[d - 1]) & MASK64
                    gd_entry.diff = delta_new
                else:
                    gd_entry.distance = ORDER_N
                    delta_new = (v_actual - self.nvq[ORDER_N - 1]) & MASK64
                    gd_entry.diff = delta_new
            else:
                if gd_entry.ctr < MAXCONF:
                    if self._forward_prob(gd_entry.ctr):
                        gd_entry.ctr += 1
                if gd_entry.u < MAXU:
                    gd_entry.u += 1
        else:
            # Tag mismatch: replaced only if u == 0 with probability 1/16 (Section III-A & III-B)
            if gd_entry.u == 0 and self.rng.random() < (1.0 / 16.0):
                d = ORDER_N
                delta_new = (v_actual - self.nvq[d - 1]) & MASK64
                self.table[idx] = TableEntry(
                    tag=t,
                    u=0,
                    ctr=0,
                    diff=delta_new,
                    distance=d,
                )

        # Propagate committed value to Non-Speculative GVQ (NVQ) in FIFO order
        self.nvq.insert(0, v_actual)
        self.nvq.pop()
        return q_entry


# ==============================================================================
# EXPERIMENT 1: Version 1 — 1-Shot In-Flight SpecGVQ Value Teleportation
# ==============================================================================
def run_experiment_1_specgvq_teleportation():
    print("=" * 78)
    print("EXPERIMENT 1: 1-Shot In-Flight SpecGVQ Value Teleportation (Normal & Deferred)")
    print("=" * 78)

    PC_ATK = 0x401080
    PC_DUMMY = 0x401000
    PC_VICTIM_SECRET = 0x509900  # Completely different PC (no index or tag collision!)
    PC_VICTIM_OTHER = 0x509920
    TARGET_D = 6                 # Secret load is 6 loads before L_atk
    SECRET_VAL = 0xDEADCAFEBEEF1337

    assert index_hash(PC_ATK) != index_hash(PC_VICTIM_SECRET)

    for mode in ["Mode 1A (ROB-Stall Normal 3c Prediction)",
                 "Mode 1B (L1D-Miss Deferred `Wake` Prediction)"]:
        sim = EgDiffPredictor(seed=101)

        # Step 1: Offline Attacker Self-Training (Zero victim interaction)
        # First step distance down from 32 to TARGET_D, then saturate ctr to 7 with diff = 0
        train_commits = 0
        while True:
            entry = sim.table[index_hash(PC_ATK)]
            # Execute TARGET_D dummy loads (value 0)
            for _ in range(TARGET_D):
                q = sim.dispatch_load(PC_DUMMY)
                sim.execute_load_in_lq(q, 0)
                sim.commit_oldest_load()
            # When distance == TARGET_D + 1, the mismatch on this commit will step
            # distance -> TARGET_D and latch diff = atk_val - NVQ[TARGET_D].
            # Therefore atk_val MUST be 0 when distance is TARGET_D + 1 OR TARGET_D!
            if not (entry.tag == tag_hash(PC_ATK) and entry.distance in (TARGET_D + 1, TARGET_D)):
                k = train_commits + 1
                atk_val = ((k * k * k + k) * 0x9E3779B97F4A7C15) & MASK64
            else:
                atk_val = 0
            q_atk = sim.dispatch_load(PC_ATK)
            sim.execute_load_in_lq(q_atk, atk_val)
            sim.commit_oldest_load()
            train_commits += 1
            if entry.tag == tag_hash(PC_ATK) and entry.distance == TARGET_D and entry.ctr == MAXCONF and entry.diff == 0:
                break

        # Clear any lastMisp window from training before calling victim
        sim.step_instructions(LAST_MISP_WINDOW)

        # Step 2: Single Victim Execution (1-Shot)
        # Victim loads SECRET_VAL once, scrubs its architectural register (x0 = 0),
        # and executes TARGET_D - 1 epilogue loads before returning to caller.
        q_secret = sim.dispatch_load(PC_VICTIM_SECRET)
        if "Mode 1A" in mode:
            # L_secret hits in L1D (Avail = 1), kept in SpecGVQ by an older ROB-head stall (e.g., Div)
            sim.execute_load_in_lq(q_secret, SECRET_VAL)

        # Epilogue loads (TARGET_D - 1 loads)
        for i in range(TARGET_D - 1):
            q_ep = sim.dispatch_load(PC_VICTIM_OTHER + i * 4)
            sim.execute_load_in_lq(q_ep, 0x1000 + i)

        # Caller dispatches L_atk (with delayed address operand)
        q_atk = sim.dispatch_load(PC_ATK)

        if "Mode 1B" in mode:
            # In Mode 1B, L_secret was an L2 miss (Avail == 0 when L_atk dispatched),
            # so L_atk bound SpecGVQ[i_sec].wake! Now L_secret returns from L2:
            assert q_secret.wake is not None, "Expected Wake pointer binding!"
            sim.execute_load_in_lq(q_secret, SECRET_VAL)

        teleported_val = q_atk.prf_dest_val
        print(f"  [{mode}]")
        print(f"    Offline attacker self-training commits : {train_commits}")
        print(f"    Victim secret loaded (1-shot)          : 0x{SECRET_VAL:016x}")
        print(f"    Value injected into Attacker PRF       : 0x{teleported_val:016x}")
        print(f"    Exact 64-bit match                     : {teleported_val == SECRET_VAL}\n")
        assert teleported_val == SECRET_VAL


# ==============================================================================
# EXPERIMENT 2: Version 2 — Post-Commit NVQ Capture & Replay (Across Fences)
# ==============================================================================
def run_experiment_2_nvq_capture_and_replay(num_trials: int = 500):
    print("=" * 78)
    print("EXPERIMENT 2: Post-Commit NVQ Capture & Replay Across Serializing Return Fences")
    print("=" * 78)

    PC_ATK = 0x402040
    PC_VICTIM_SECRET = 0x608800
    PC_VICTIM_EPILOGUE = 0x608840
    TARGET_D = 8
    SECRET_VAL = 0x0123456789ABCDEF

    calls_needed_list = []
    success_count = 0

    for trial in range(num_trials):
        sim = EgDiffPredictor(seed=1000 + trial)

        # Step 1: Offline pre-positioning of gdEntry.distance to TARGET_D + 1
        # Must use a NON-LINEAR sequence (e.g. quadratic/cubic), because a linear
        # sequence v_k = k * C has constant difference 16*C at d = 16 and locks at d = 16!
        while True:
            entry = sim.table[index_hash(PC_ATK)]
            q = sim.dispatch_load(PC_ATK)
            k = sim.seq_counter
            val = ((k * k * k + k) * 0x9E3779B97F4A7C15) & MASK64
            sim.execute_load_in_lq(q, val)
            sim.commit_oldest_load()
            if entry.tag == tag_hash(PC_ATK) and entry.distance == TARGET_D + 1:
                break

        # Step 2: Victim Calls with Full Pipeline-Draining Fence (`dsb`/`lfence`) Before Return
        # Notice every load inside the victim commits and leaves SpecGVQ BEFORE returning!
        victim_calls = 0
        while True:
            victim_calls += 1
            # Victim loads secret and commits
            q_sec = sim.dispatch_load(PC_VICTIM_SECRET)
            sim.execute_load_in_lq(q_sec, SECRET_VAL)
            sim.commit_oldest_load()

            # Victim executes TARGET_D - 1 epilogue loads and commits (e.g. before fence)
            for i in range(TARGET_D - 1):
                q_ep = sim.dispatch_load(PC_VICTIM_EPILOGUE + i * 4)
                sim.execute_load_in_lq(q_ep, 0x7FFF0000 + i)
                sim.commit_oldest_load()

            # SpecGVQ is 100% empty here (fence drained the pipeline)!
            assert len(sim.spec_gvq) == 0

            # Caller executes L_atk loading 0 and commits
            q_atk = sim.dispatch_load(PC_ATK)
            sim.execute_load_in_lq(q_atk, 0)
            sim.commit_oldest_load()

            entry = sim.table[index_hash(PC_ATK)]
            if entry.ctr == MAXCONF:
                break

        calls_needed_list.append(victim_calls)
        sim.step_instructions(LAST_MISP_WINDOW)

        # Step 3: Non-Destructive Branch-Shadow Replay in Attacker Code (Zero Victim Calls)
        # Leak all 8 bytes of the 64-bit secret across 8 replay iterations without resetting ctr!
        reconstructed_secret = 0
        for byte_idx in range(8):
            older_branch_seq = sim.seq_counter
            # Put a known base value (0) at distance TARGET_D in SpecGVQ
            for _ in range(TARGET_D):
                q_base = sim.dispatch_load(0x403000)
                sim.execute_load_in_lq(q_base, 0)

            # Dispatch L_atk inside the branch shadow (address delayed past branch resolution)
            q_replay = sim.dispatch_load(PC_ATK)
            assert q_replay.vp == 1, "Expected valid prediction on replay!"
            predicted_neg_s = q_replay.prf_dest_val
            recovered_s = (-predicted_neg_s) & MASK64
            leaked_byte = (recovered_s >> (8 * byte_idx)) & 0xFF
            reconstructed_secret |= (leaked_byte << (8 * byte_idx))

            # Older branch resolves at cycle 21 and squashes L_atk before LQ verification/commit!
            sim.branch_squash_after(older_branch_seq)

            # Verify gdEntry.ctr is STILL 7 and diff is STILL -SECRET_VAL!
            assert sim.table[index_hash(PC_ATK)].ctr == MAXCONF

        if reconstructed_secret == SECRET_VAL:
            success_count += 1

    sorted_calls = sorted(calls_needed_list)
    p50 = sorted_calls[len(sorted_calls) // 2]
    p90 = sorted_calls[int(len(sorted_calls) * 0.90)]
    p99 = sorted_calls[int(len(sorted_calls) * 0.99)]

    print(f"  Trials run                               : {num_trials}")
    print(f"  Full 64-bit secret recovery success rate : {success_count}/{num_trials} ({100.0*success_count/num_trials:.1f}%)")
    print(f"  Victim calls to capture & saturate FPC   : mean = {statistics.mean(calls_needed_list):.1f}, "
          f"median = {p50}, 90th = {p90}, 99th = {p99}")
    print(f"  Non-destructive 8-byte branch-shadow leak: ctr remained 7 across all 8 byte extractions\n")


# ==============================================================================
# EXPERIMENT 3: Testing the "Decoy Density" Objection
# ==============================================================================
def run_experiment_3_decoy_density():
    print("=" * 78)
    print("EXPERIMENT 3: Effect of Constant-Value 'Decoy Density' in 32-Load Window")
    print("=" * 78)

    PC_ATK = 0x405000
    PC_VICTIM_LOAD_BASE = 0x701000
    TARGET_D = 10
    SECRET_VAL = 0xA5A5F0F012345678
    NUM_TRIALS = 100

    print(f"  Secret sits at distance d = {TARGET_D} (22 older slots d = 11..32 can contain constant decoys)")
    print(f"  {'Decoy Density':<18} | {'Naive Attacker (Fixed v_actual=0)':<35} | {'Our Attacker (Pre-step / Modulate v_actual)':<42}")
    print("  " + "-" * 101)

    for num_decoys in [0, 1, 5, 10, 20, 31]:
        naive_ok = 0
        smart_ok = 0

        for trial in range(NUM_TRIALS):
            # Choose which of the 31 non-secret slots in the 32-load window are constant decoys
            rng = random.Random(5000 + trial)
            non_secret_distances = [d for d in range(1, 33) if d != TARGET_D]
            # Always place at least one decoy at d > TARGET_D when num_decoys >= 1
            higher_distances = [d for d in non_secret_distances if d > TARGET_D]
            lower_distances = [d for d in non_secret_distances if d < TARGET_D]
            ordered_candidates = higher_distances + lower_distances
            decoy_set = set(ordered_candidates[:num_decoys])

            # --- Run 1: Naive Attacker (polls from 32 holding v_actual = 0 fixed) ---
            sim_naive = EgDiffPredictor(seed=2000 + trial)
            for call in range(80):
                # 32 loads before L_atk (d = 32 down to 1)
                for d in range(32, 0, -1):
                    q = sim_naive.dispatch_load(PC_VICTIM_LOAD_BASE + d * 4)
                    if d == TARGET_D:
                        val = SECRET_VAL
                    elif d in decoy_set:
                        val = 0x7FFFFFFF0000 + d * 8  # Constant decoy across all calls!
                    else:
                        val = rng.getrandbits(64)     # Varying non-decoy load
                    sim_naive.execute_load_in_lq(q, val)
                    sim_naive.commit_oldest_load()

                q_atk = sim_naive.dispatch_load(PC_ATK)
                sim_naive.execute_load_in_lq(q_atk, 0)
                sim_naive.commit_oldest_load()

                entry = sim_naive.table[index_hash(PC_ATK)]
                if entry.tag == tag_hash(PC_ATK) and entry.ctr == MAXCONF:
                    break

            entry_naive = sim_naive.table[index_hash(PC_ATK)]
            if entry_naive.tag == tag_hash(PC_ATK) and entry_naive.ctr == MAXCONF and ((-entry_naive.diff) & MASK64) == SECRET_VAL:
                naive_ok += 1

            # --- Run 2: Our Attacker (modulates v_actual until d == TARGET_D, then holds v_actual = 0) ---
            sim_smart = EgDiffPredictor(seed=3000 + trial)
            for call in range(160):
                for d in range(32, 0, -1):
                    q = sim_smart.dispatch_load(PC_VICTIM_LOAD_BASE + d * 4)
                    if d == TARGET_D:
                        val = SECRET_VAL
                    elif d in decoy_set:
                        val = 0x7FFFFFFF0000 + d * 8  # Constant decoy across all calls!
                    else:
                        val = rng.getrandbits(64)
                    sim_smart.execute_load_in_lq(q, val)
                    sim_smart.commit_oldest_load()

                entry = sim_smart.table[index_hash(PC_ATK)]
                # When distance is TARGET_D + 1, the mismatch steps distance -> TARGET_D
                # and latches diff = v_atk - NVQ[TARGET_D]. So v_atk must be 0 at
                # BOTH TARGET_D + 1 (the latching commit) and TARGET_D (the FPC training commits)!
                if not (entry.tag == tag_hash(PC_ATK) and entry.distance in (TARGET_D + 1, TARGET_D)):
                    k = call + 1
                    v_atk = ((k * k * k + k) * 0x9E3779B97F4A7C15) & MASK64
                else:
                    v_atk = 0

                q_atk = sim_smart.dispatch_load(PC_ATK)
                sim_smart.execute_load_in_lq(q_atk, v_atk)
                sim_smart.commit_oldest_load()

                entry = sim_smart.table[index_hash(PC_ATK)]
                if entry.tag == tag_hash(PC_ATK) and entry.distance == TARGET_D and entry.ctr == MAXCONF:
                    break

            entry_smart = sim_smart.table[index_hash(PC_ATK)]
            if entry_smart.tag == tag_hash(PC_ATK) and entry_smart.ctr == MAXCONF and ((-entry_smart.diff) & MASK64) == SECRET_VAL:
                smart_ok += 1

        pct_str = f"{num_decoys}/31 ({100.0*num_decoys/31:.0f}%)"
        print(f"  {pct_str:<18} | {naive_ok:>3}/{NUM_TRIALS} ({100.0*naive_ok/NUM_TRIALS:>5.1f}%){'':<16} | "
              f"{smart_ok:>3}/{NUM_TRIALS} ({100.0*smart_ok/NUM_TRIALS:>5.1f}%)")
    print()


# ==============================================================================
# EXPERIMENT 4: The Real Bottleneck for Version 2 — Dynamic Load-Count Jitter
# ==============================================================================
def run_experiment_4_load_jitter_sensitivity():
    print("=" * 78)
    print("EXPERIMENT 4: Real Bottleneck for Version 2 (NVQ) — Dynamic Load-Count Jitter")
    print("=" * 78)

    PC_ATK = 0x406000
    PC_VICTIM_SECRET = 0x801000
    PC_VICTIM_EP = 0x802000
    TARGET_D = 8
    SECRET_VAL = 0xCAFEBABE11223344
    MAX_CALLS_BUDGET = 300
    NUM_TRIALS = 200

    print(f"  Budget = {MAX_CALLS_BUDGET} victim calls; measuring probability of saturating ctr=7 on d={TARGET_D}")
    print(f"  {'Per-Call Load Jitter Prob':<28} | {'NVQ Capture Success Rate':<28} | {'Avg Calls (When Successful)':<28}")
    print("  " + "-" * 90)

    for jitter_prob in [0.0, 0.02, 0.05, 0.10, 0.20]:
        successes = 0
        calls_list = []

        for trial in range(NUM_TRIALS):
            sim = EgDiffPredictor(seed=7000 + trial)
            rng = random.Random(8000 + trial)

            # Pre-position distance to TARGET_D + 1 offline (using non-linear sequence)
            while True:
                entry = sim.table[index_hash(PC_ATK)]
                q = sim.dispatch_load(PC_ATK)
                k = sim.seq_counter
                sim.execute_load_in_lq(q, ((k * k * k + k) * 0x9E3779B97F4A7C15) & MASK64)
                sim.commit_oldest_load()
                if entry.tag == tag_hash(PC_ATK) and entry.distance == TARGET_D + 1:
                    break

            for call in range(1, MAX_CALLS_BUDGET + 1):
                # With probability `jitter_prob`, an extra dynamic load executes in the victim epilogue
                extra_load = 1 if rng.random() < jitter_prob else 0
                epilogue_loads = (TARGET_D - 1) + extra_load

                q_sec = sim.dispatch_load(PC_VICTIM_SECRET)
                sim.execute_load_in_lq(q_sec, SECRET_VAL)
                sim.commit_oldest_load()

                for i in range(epilogue_loads):
                    q_ep = sim.dispatch_load(PC_VICTIM_EP + i * 4)
                    sim.execute_load_in_lq(q_ep, 0x55550000 + i)
                    sim.commit_oldest_load()

                # If distance was knocked off TARGET_D by jitter, attacker modulates v_actual to step back around
                entry = sim.table[index_hash(PC_ATK)]
                if entry.distance != TARGET_D and entry.distance != TARGET_D + 1:
                    v_atk = call * 0x77777
                else:
                    v_atk = 0

                q_atk = sim.dispatch_load(PC_ATK)
                sim.execute_load_in_lq(q_atk, v_atk)
                sim.commit_oldest_load()

                if entry.ctr == MAXCONF and entry.distance == TARGET_D and ((-entry.diff) & MASK64) == SECRET_VAL:
                    successes += 1
                    calls_list.append(call)
                    break

        avg_c = f"{statistics.mean(calls_list):.1f}" if calls_list else "N/A"
        print(f"  {100.0*jitter_prob:>6.1f}%{'':<21} | {successes:>3}/{NUM_TRIALS} ({100.0*successes/NUM_TRIALS:>5.1f}%){'':<12} | {avg_c:<28}")
    print()


if __name__ == "__main__":
    run_experiment_1_specgvq_teleportation()
    run_experiment_2_nvq_capture_and_replay()
    run_experiment_3_decoy_density()
    run_experiment_4_load_jitter_sensitivity()
