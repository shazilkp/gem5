# A4 — EgDiff forwards a `crypto_verify32` key byte across the call boundary

**Status: PASS (36/36 checks).** `tests/test-progs/egdiff_attack/run_layer6.sh`
builds the target, measures the real distances in the binary, runs the attack
and both controls, and asserts recovery. The same gate is the reproduction
recipe.

This is the "real target in gem5" experiment: Monocypher's constant-time
`crypto_verify32()` (commit `f8bb8dbd`, vendored under
`experiment/egdiff_eval/prevalence/third_party/Monocypher/`) is called from a
target translation unit that owns the 32-byte key; the attacker translation
unit never names the key and recovers the last key word byte by byte through
EgDiff's global load-value forwarding.

---

## 1. Ownership split

- `tests/test-progs/egdiff_attack/target_verify32.c` owns the key as
  `static const uint8_t v32_key_storage[32]` and exposes
  `int target_verify32(const uint8_t guess[32])`, which tail-calls the real
  `crypto_verify32(guess, key)`.
- `tests/test-progs/egdiff_attack/poc_verify32.c` (the attacker) references
  only `target_verify32`, `target_verify32_selfcheck` and the post-hoc
  `oracle_report_verify32`. It contains no key symbol, no key-sized buffer that
  is architecturally filled from the target, and no data flow from the oracle
  back into the measurement loop.
- Mechanical checks in the gate:
  - `nm -u` on the attacker object: no `secret_` / `v32_key` undefined symbol.
  - `nm` on the binary: no **global** `secret_` / `v32_key` symbol;
    `v32_key_storage` exists only as a local read-only symbol (`r`).

**Guess ≠ key, and why that is sound.** The task text suggested passing
`guess == key` so the comparison does not early-exit. `crypto_verify32` is
Monocypher's *constant-time* comparison: `x32()` expands to `x16()` and reads
all 32 bytes of both operands unconditionally, and `neq0()` is branchless.
There is no early exit, so the guess *content* cannot move the key's load PCs or
their distances. The attacker cannot name the key under the ownership split, so
the harness passes a fixed attacker-chosen guess (all zeros) and the gate
re-measures `d` with a second, different guess pattern and asserts the distance
map is identical (`PROOF guess-invariance`). This is a deliberate, documented
deviation from the wording, not a weakening of the check: the experimenter-side
`target_verify32_selfcheck()` separately checks `crypto_verify32(key, key) == 0`.

---

## 2. Measured distance `d` in *this* binary

Tool: `tests/test-progs/egdiff_attack/measure_d.py`, which runs
`qemu-riscv64 -singlestep -d exec` on the binary's `measure` mode and intersects
the guest PC stream with the load PCs parsed from `objdump -d`. The `measure`
mode executes the *same* `victim_then_marker()` function as the attack, so the
measured distance is the attack's distance.

Static proof that the last 8 byte loads read the **key** (not the guess):

```
target_verify32:
    add  a1,a1,-1168   # 5ab70 <v32_key_storage>     (a1 = 2nd arg = key)
    j    crypto_verify32
crypto_verify32:
    mv   s1,a1                                        (key register)
    ...
    add  a0,s1,24      # last load64_le call base = key+24
    jal  load64_le
load64_le (8 byte loads, execution order): 1,0,2,3,4,5,6,7
```

Dynamic proof that the marker is the first load after the callee returns:
`loads_between_ret_and_marker=0`. In the same run, `victim_then_marker`'s own
`ld ra,8(sp)` frame restore is emitted *after* the marker call, so it is outside
the window (also checked statically by the gate).

Measured map (identical with two different guesses; `d` is the number of loads
from the key byte load to the marker, 1-based):

| key byte | offset | load PC | measured d | recovered |
|---|---|---|---|---|
| key[31] | 31 | `0x10f7e` `lbu a0,7(a0)` | **7** | `0xa1` |
| key[30] | 30 | `0x10f74` `lbu a5,6(a0)` | **8** | `0x10` |
| key[29] | 29 | `0x10f6a` `lbu a4,5(a0)` | **9** | `0x8f` |
| key[28] | 28 | `0x10f64` `lbu a3,4(a0)` | **10** | `0x2e` |
| key[27] | 27 | `0x10f60` `lbu a2,3(a0)` | **11** | `0xbd` |
| key[26] | 26 | `0x10f5c` `lbu a1,2(a0)` | **12** | `0x4c` |
| key[24] | 24 | `0x10f58` `lbu a5,0(a0)` | **13** | `0x6a` |
| key[25] | 25 | `0x10f54` `lbu a6,1(a0)` | **14** | `0xdb` |

(`SUMMARY n=8 d_min=7 d_max=14`.)

PCs are build-dependent: a fresh rebuild of the same sources measured
`0x10f60`/`0x10f86` for the two quoted epilogue loads while the `d` map and the
recovered bytes were unchanged. Treat the PC column as one build's disassembly.

The measured 7..14 matches the prevalence study's `t_verify32` table exactly
(the same compiler, same Monocypher source and the same `load64_le` epilogue).
The distances are shifted by the six `ld` stack-restore loads in
`crypto_verify32`'s epilogue: the last key byte is 1 load before the caller's
next load plus those six, so `d = 7`, and the earliest bytes of the word are
`d = 13`/`14`. Note that the per-byte execution order inside the word is
`1,0,2,3,4,5,6,7`, hence the scrambled offset/d pairing above.

---

## 3. Training at the measured `d`

Mode 1 of `poc_v1` only trains `d = 1` with a two-load pair. The Layer 6
harness generalises it, one measured distance at a time:

```
per training iteration:
    v = mix(i)                  (varies per iteration)
    carry_buf[0] = v
    s += carry_buf[0]           load 1 of d   (carrier, value v)
    s += noise[(i+j)&63]        loads 2..d    (independently varying)
    atk_buf[0] = v              store: does not advance EgDiff's load distance
    s += atk_load(atk_buf,0)    marker, exactly d loads after the carrier
```

Only distance `d` has a constant difference (`marker - carrier == 0`). For every
other distance the difference is a varying mix/noise value, and across
iterations the marker also differs from the previous iteration's marker and
carrier, so no self-distance traps the poller. Distance polling walks
`32 → … → d` and latches `diff = 0`; `TRAIN = 256` iterations are far more than
the ~18 polling steps plus the FPC ramp needed for `d = 14`.

At measurement, per attempt: train, freeze `atk_buf` to the sentinel by stores
(no load), evict the probe array, `fence.i` drain, call
`target_verify32(guess)` once, then execute the marker load with its address
delayed by 12 register-only divisions. The architectural sentinel value routes
the post-squash probe to the reserved slot `LINES`, outside the 256 measured
lines, so no byte index is ever excluded and the architectural re-execution is
the per-attempt hot/cold calibration reference (poc_v1's estimator):
`thresh = (t_arch + t_cold)/2`, exactly one non-architectural line below the
threshold casts a vote, a byte needs `MIN_VOTES = 2` and a 2× margin over the
runner-up, otherwise its `known` bit stays clear.

---

## 4. Result

One row per run, deterministic (`EGDIFF_SEED=1`), 16 attempts per byte:

```
EgDiff on : key_word=a1108f2ebd4cdb6a recovered=a1108f2ebd4cdb6a
            known=ff bytes=8/8 lane_base=24 arch=OK MATCH
            (all 8 lanes: votes=16)
EgDiff off: recovered=0000000000000000 known=00 bytes=0/8 arch=OK NO-MATCH
FLOP LVP  : recovered=0000000000000000 known=00 bytes=0/8 arch=OK NO-MATCH
```

The recovered word `a1108f2ebd4cdb6a` is key bytes 24..31
(`6a db 4c bd 2e 8f 10 a1` little-endian). **8/8 bytes recovered.**

Controls:

- `EGDIFF_ON=0`: 0/8, `known=00`, every `egdiff*` statistic exactly zero,
  identical `sink` accumulator (not a checksum; see the note below).
- `EGDIFF_TYPE=flop`: 0/8, `known=00`, every `egdiff*` statistic zero,
  `numPredictions > 0` (the FLOP local predictor is active but its
  pseudo-random training-value sequence never locks, so no byte is forwarded),
  identical `sink` accumulator.
- Architectural precondition: `archpre self=OK repro=OK marker=OK` — the
  target is self-consistent, `target_verify32(guess)` is deterministic for a
  fixed guess, and every attempt's re-executed marker returned the sentinel.

Representative EgDiff statistics for the `on` run:

```
egdiffPredictions        109042
egdiffDeferredPredictions 170482
egdiffNVQBases            29896
egdiffTableAllocations      247
egdiffPollingSteps       348433
numPredictionCorrect     278392
numPredictionIncorrect      369
```

The incorrect predictions are the measured marker loads (predicted key byte vs.
architectural sentinel), which is the expected squash path; the harness retrains
after each such squash, and each lane still received 16/16 informative votes.

`sink=14903399325723612160` is identical in all three runs. It is a **harness
accumulator**, not an architectural checksum of the victim: it sums training
values, a probe read that is `0` on the architectural sentinel path, and the
marker's architectural return. It is therefore identical by construction and
only detects gross divergence in the training loop. The real architectural
controls are `archpre self=OK repro=OK marker=OK` (the re-executed marker
returns the sentinel, so the measurement path did not disappear) and the
oracle's `MATCH` on the recovered value.

---

## 5. Expected negative — `crypto_chacha20`

Not re-run in this harness (optional item). The structural argument is recorded
here from the prevalence study's `t_chacha20` measurement of the same Monocypher
commit: the 32-byte key is expanded into the ChaCha state once at function
entry, then hundreds of derived-state/message loads follow. The measured
distance from the last key access to the caller's next load is **modal 519,
minimum 85** over 900 calls. Since EgDiff's window is `d ≤ 32`, no key-buffer
load is reachable, and the best case (85) is still 2.65× outside the window.
Unlike `verify32`'s short constant-time exit tail, this is a structural failure
of the precondition, not a harness limitation. (See
`experiment/egdiff_eval/prevalence/results/prevalence.md`, `t_chacha20`.)

---

## 6. Reproduction commands

```bash
cd /home/shazil/project/gem5
D=tests/test-progs/egdiff_attack
MONO=experiment/egdiff_eval/prevalence/third_party/Monocypher/src

# 1. build (attacker + target + the real Monocypher)
riscv64-linux-gnu-gcc -static -O2 -fno-pic -no-pie -fno-ipa-icf -I"$MONO" \
    -o "$D/poc_verify32" "$D/poc_verify32.c" "$D/target_verify32.c" \
    "$MONO/monocypher.c"

# 2. measure d in this binary (prints the D <offset> <d> table + proofs)
python3 "$D/measure_d.py" "$D/poc_verify32"

# 3. attack (measured map from step 2)
EGDIFF_TYPE=egdiff EGDIFF_ON=1 build/RISCV/gem5.opt -d /tmp/opencode/a4/on \
    configs/example/egdiff.py --cpu-type=O3CPU --caches \
    --cmd="$PWD/$D/poc_verify32" \
    --options="attack 24:13,25:14,26:12,27:11,28:10,29:9,30:8,31:7"

# 4. controls
EGDIFF_TYPE=egdiff EGDIFF_ON=0 build/RISCV/gem5.opt -d /tmp/opencode/a4/off \
    configs/example/egdiff.py --cpu-type=O3CPU --caches \
    --cmd="$PWD/$D/poc_verify32" \
    --options="attack 24:13,25:14,26:12,27:11,28:10,29:9,30:8,31:7"
EGDIFF_TYPE=flop EGDIFF_ON=1 build/RISCV/gem5.opt -d /tmp/opencode/a4/flop \
    configs/example/egdiff.py --cpu-type=O3CPU --caches \
    --cmd="$PWD/$D/poc_verify32" \
    --options="attack 24:13,25:14,26:12,27:11,28:10,29:9,30:8,31:7"

# 5. full gate (build + ownership + topology + measure + 3 runs + asserts)
tests/test-progs/egdiff_attack/run_layer6.sh
```

Note: `se.py` does not split `--cmd` on spaces, so the lane map is passed with
`--options=...` (which `se.py` splits on `;` and whitespace).

---

## 7. Caveats

- **`d` is an architectural (QEMU) distance.** As in the prevalence study, it
  counts architectural loads; wrong-path loads injected by a mispredicted
  branch would not appear. Here there is no branch between the last key load
  and the marker (`crypto_verify32` is branchless and `victim_then_marker`
  contains no conditional code), so the distance is also the dispatch-order
  distance. The probabilistic 1/16 cold allocation and the 1024-instruction
  `lastMisp` suppression are the only sources of run-to-run variation in the
  predictor, and both are deterministic for `EGDIFF_SEED=1`.
- **Lane mapping is public code structure, not key material.** `lane_base = 24`
  (the last key word) and the per-byte distance table are derived from the
  binary by `measure_d.py`; the gate passes that map to the attacker. The
  offsets of a 32-byte key are public ABI information. The oracle alone reads
  the key *values*.
- **Guess content.** See §1: the attack uses a fixed non-key guess. The
  invariance of `d` across two guesses is measured by the gate. The wording
  "guess equal to the key" is not implementable under the mandatory ownership
  split and is unnecessary for a constant-time comparison.
- **What is claimed.** Same-process "cannot name it" model (as in Layer 5 and
  FLOP's engine boundary), not MMU-enforced isolation. The attacker's
  instruction stream never loads the key; the value reaches an attacker register
  only through EgDiff's forwarding.
- **Per-attempt retraining is intentional.** The architectural re-execution of
  the mispredicted marker commits the sentinel and re-polls the marker's table
  entry, so the entry must be retrained for every attempt. `REPS = 16` per byte.
- **Build artifacts** `tests/test-progs/egdiff_attack/poc_verify32` are produced
  by the gate in the source tree, matching the existing Layer 4/5 scripts.
