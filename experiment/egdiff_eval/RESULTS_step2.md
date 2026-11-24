# Layer 5 — Real-library SipHash-2-4 key extraction (step 2 results)

**Branch:** `feat/egdiff-attack`
**Simulator:** `build/RISCV/gem5.opt` (gem5 25.1.0.1, RV64)
**Compiler:** `riscv64-linux-gnu-gcc -static -O2 -fno-pic -no-pie`
**Binary:** `tests/test-progs/egdiff_attack/siphash_attack`
**Gate:** `tests/test-progs/egdiff_attack/run_layer5.sh`
**Source:** `tests/test-progs/egdiff_attack/siphash_attack.c`

This step replaces the synthetic `secret_box` of Layer 4 with a real library
routine: the widely used reference SipHash-2-4 implementation
(`siphash24(const void *src, unsigned long src_sz, const uint8_t key[16])`,
Marek Majkowski's public `csiphash`, MIT licence).  The library is essentially
verbatim: the key parameter type is `uint8_t`, the `_le64toh` conversions are
elided (they are the identity on little-endian RISC-V), and a
`noinline, noclone, noipa` attribute is added.  The attribute is required for
the measurement: without it GCC constant-propagates `src_sz == 0` into the
function, removes the reference implementation's switch and literal-pool
loads, and the measured load distances collapse.  The algorithm itself is
untouched.

## 1. Result

`EGDIFF_TYPE=egdiff EGDIFF_ON=1` recovers all 16 key bytes.  The automated gate
`run_layer5.sh` reports **22/22 checks passed, `LAYER 5 PASSED`**:

```
== result: 22 passed, 0 failed ==
LAYER 5 PASSED
```

```
siphash_attack d_k1=5 d_k0=6
siphash_attack k0=0123456789abcdef k1=f1e2d3c4b5a69788 \
               rk0=0123456789abcdef rk1=f1e2d3c4b5a69788 \
               bytes=16/16 votes=79/80 arch=OK sink=8976677841604280768 MATCH
```

Key bytes in memory order (`key[0..15]`, little-endian key words):

```
ef cd ab 89 67 45 23 01  88 97 a6 b5 c4 d3 e2 f1
```

Both 64-bit words (`k0 = 0x0123456789abcdef`, `k1 = 0xf1e2d3c4b5a69788`) are
recovered byte-for-byte.

The two negative controls use the **same binary**:

| case | `rk0` | `rk1` | bytes | votes | verdict |
|---|---|---|---|---|---|
| `egdiff`, on  | `0123456789abcdef` | `f1e2d3c4b5a69788` | 16/16 | 79/80 | **MATCH** |
| `egdiff`, off | `0000000000000000` | `0000000000000000` | 0/16 | 0/80 | NO-MATCH |
| `flop`, on    | `0000000000000000` | `0000000000000000` | 0/16 | 0/80 | NO-MATCH |

All three runs report `arch=OK` and the **same architectural checksum**
`sink=8976677841604280768`, i.e. the architectural execution is identical and
the controls genuinely execute the same code; only the forwarded value differs.

Predictor statistics, `EgDiff` run (`stats.txt`):

```
egdiffPredictions        181850   (values forwarded at dispatch)
egdiffNVQBases            59775   (base came from the committed NVQ)
egdiffDeferredPredictions  5531   (deferred Wake path; also exercised)
egdiffDeferredIgnored     19837
numPredictionIncorrect      506   (includes the 80 measurement mispredictions)
numPredictionCorrect     184888
```

`EGDIFF_ON=0`: every `loadValuePredictor.egdiff*` statistic is `0` and
`numPredictions = 0`.  `EGDIFF_TYPE=flop`: every `egdiff*` statistic is `0`,
while the FLOP local predictor itself is active (`numPredictions = 90968`).
Neither control produces a hot probe line, so no byte is accepted.

## 2. Dynamic load-distance analysis (`objdump -d`)

`riscv64-linux-gnu-objdump -d tests/test-progs/egdiff_attack/siphash_attack`
around `<siphash24>`.  With `src_sz = 0`, the message loop and the switch
case bodies are skipped; the executed load stream is:

| # | instruction | meaning |
|---|---|---|
| 1 | `ld a6,1000(a4)` | SipHash round constant (literal pool) |
| 2 | `ld a7,0(a2)`   | **k0** (`key[0..7]`) |
| 3 | `ld a3,8(a2)`   | **k1** (`key[8..15]`) |
| 4 | `ld a2,1008(a4)` | round constant |
| 5 | `ld a4,1016(a4)` | round constant |
| 6 | `ld a4,1024(a4)` | round constant |
| 7 | `lw a4,0(a1)`   | `switch (src_sz)` jump-table entry |
| 8 | `ld ...` inside `<atk_load>` | **`L_atk`** (caller's next load) |

Therefore

```
distance(k1, L_atk) = 8 - 3 = 5
distance(k0, L_atk) = 8 - 2 = 6
```

Both are `<= ORDER_N = 32`.  There are **four** loads between `k1` and the
`ret` (the four round constants) plus the jump-table load after the branch;
there is **no** load between the `ret` and `L_atk` (`delayed_atk_load` is
load-free; see §3).  The key words are exactly two 64-bit `ld`s because
`csiphash` uses `const uint64_t *_key = (const uint64_t *)key;`.

The intervening loads are constant-valued / control loads, not
secret-dependent: three literal-pool round constants after `k1` (the fourth is
loaded before `k0`) and the switch jump-table entry.  They do not create
extra fixed points of distance polling **during training**, because training
is a separate body that never calls SipHash.  During the measurement they only
affect the normal verification/re-execution; the entry is retrained for every
attempt anyway (§3, one capture per byte).

`run_layer5.sh` re-checks the compiled topology before running:
- `k0` is the 2nd load, `k1` the 3rd;
- the `src_sz == 0` switch path executes exactly one load (the jump table);
- `<delayed_atk_load>` contains **0 loads** and **12 `divu`s**;
- `<atk_load>` contains exactly one static load.

If a different compiler/version changes this layout, the gate's disassembly
checks and the end-to-end recovery fail loudly; the constants `DIST_K1` and
`DIST_K0` in the source are the only place that needs updating.

## 3. Harness design

### 3.1 One static instruction for training and measurement
`L_atk` is the single load inside

```c
__attribute__((noinline, noclone))
static uint64_t atk_load(volatile uint64_t *base, uint64_t idx)
{
    return base[idx & 63];
}
```

Training passes `carry_buf`, measurement passes `atk_buf`; the load PC is the
same in both.  Both are `volatile`, so the load is never CSE'd.  `atk_buf` is
initialised to `SENTINEL` (`0xAAAAAAAAAAAAAAAA`).

### 3.2 Training to a unique fixed point (the poller cannot lock elsewhere)
Distance polling in Algorithm 1 locks onto the *first* distance whose
difference is constant.  The training body per iteration is (for `d = 5`):

```
A B C X Y Z W L
```

where `C` is a *carrier* load of the value `v_i`, `L` is `L_atk` loading the
same value, and there are exactly `d-1` varying loads between them.  Every
other load takes an independently varying value (`mix(i)` of the iteration
counter, or a noise table indexed by `i & 63`).  Concretely the compiled
`train_d5` loop executes 12 loads/iteration:

```
4 loop-invariant literal/stack loads (constant values)
ld noiseA   ld noiseB
ld carrier   <-- value v_i, distance 5 before L_atk
ld noiseC   ld noiseD   ld noiseE   ld noiseF
ld inside atk_load  <-- v_i
```

Only `d = 5` has a constant difference (`v_i - v_i = 0`).  The four constant
loads are **not** fixed points: their difference is `v_i - const`, which varies
with `i`; the same holds for every distance reaching a previous iteration
(`v_i - v_{i-k}`).  `d = 6` uses one more noise load.  The carrier/load pair
is therefore the sole stable stride, and polling walks `32 -> ... -> 5`
(or `6`), latches `diff = 0`, and saturates `ctr = 7` (`u = 3`).

The training loop runs `TRAIN = 320` iterations, comfortably more than the
~1/16 geometric cold allocation (mean 16) plus ≤32 polling steps plus the
expected ~41 FPC matches needed to reach `ctr = 7`.

### 3.3 Measurement: hot key, delayed address, speculative probe
Per attempt:

1. `flush_l1()` (a 64 KiB flush buffer; the L1D is 64 KiB, 2-way).
2. Retrain `L_atk` to `d`.
3. Touch the key line (`g_sink += *(volatile uint8_t *)secret_key;`).  This
   load is **before** the SipHash call, so it cannot sit between the key load
   and `L_atk`; it makes the key L1-resident so the base value is already
   available at `L_atk` dispatch (SpecGVQ-`avail` or committed NVQ) and the
   prediction is forwarded immediately.
4. Call `siphash24(empty_msg, 0, secret_key)` exactly once.
5. Call `delayed_atk_load(tag)`: twelve real register-only `divu`s derive the
   index (opaque to the compiler), then `atk_load` issues.  The address is
   ready only after the divisions, while the dependent probe below issues
   immediately with the forwarded key word.
6. `val = ...`; the dependent probe reads
   `probe[slot * 64]`, where

   ```c
   byte = (val >> (8 * b)) & 0xff;
   slot = (val == SENTINEL) ? LINES : byte;   /* LINES = 256 */
   ```

   The speculative path (predicted key byte) fills one of the 256 measured
   lines; the post-squash architectural path (`val == SENTINEL`) fills the
   dedicated slot 256, **outside** the measured range.
7. Scan the 256 lines with `rdcycle`.  A vote is cast only if the fastest line
   beats the runner-up by `MARGIN = 20` cycles (observed: 14 vs 114 cycles,
   margin 100).

A byte is accepted only if it wins a strict majority of the 5 attempts and
the architectural precondition holds:

```
val == SENTINEL  &&  atk_buf[0] == SENTINEL  &&
secret_key unchanged  &&  siphash24() reproducible
```

No value is hard-coded and no byte value is special-cased.

### 3.4 Why the key is warmed (and what that means)
If the key line is flushed, the base load is in flight when `L_atk` dispatches
and the model binds a deferred `Wake`.  The dependent probe then issues only
when the key load completes, at roughly the same time as the division chain
starts, and occasionally the `L_atk` fill won the race and squashed the probe
(observed in early runs: some attempts produced no hot line at all).  Warming
the key line removes that race: the base is available at dispatch, the probe
issues at rename time, and the address is delayed by 12 divisions.  This is
still the same EgDiff vulnerability (a value loaded inside the library is
forwarded across the call boundary); it uses the normal/committed-NVQ forward
rather than the deferred path.  The stats show both paths fire
(`egdiffPredictions`, `egdiffNVQBases`, and `egdiffDeferredPredictions`).

## 4. Exact commands

```bash
# Build
riscv64-linux-gnu-gcc -static -O2 -fno-pic -no-pie \
  -o tests/test-progs/egdiff_attack/siphash_attack \
  tests/test-progs/egdiff_attack/siphash_attack.c

# Inspect the load distances
riscv64-linux-gnu-objdump -d tests/test-progs/egdiff_attack/siphash_attack \
  | sed -n '/<siphash24>:/,/^$/p'
riscv64-linux-gnu-objdump -d tests/test-progs/egdiff_attack/siphash_attack \
  | sed -n '/<delayed_atk_load>:/,/<atk_load>:/p'

# The three cases
EGDIFF_TYPE=egdiff EGDIFF_ON=1 build/RISCV/gem5.opt -d /tmp/opencode/l5_on \
  configs/example/egdiff.py --cpu-type=O3CPU --caches \
  --cmd=$PWD/tests/test-progs/egdiff_attack/siphash_attack
EGDIFF_TYPE=egdiff EGDIFF_ON=0 build/RISCV/gem5.opt -d /tmp/opencode/l5_off \
  configs/example/egdiff.py --cpu-type=O3CPU --caches \
  --cmd=$PWD/tests/test-progs/egdiff_attack/siphash_attack
EGDIFF_TYPE=flop EGDIFF_ON=1 build/RISCV/gem5.opt -d /tmp/opencode/l5_flop \
  configs/example/egdiff.py --cpu-type=O3CPU --caches \
  --cmd=$PWD/tests/test-progs/egdiff_attack/siphash_attack

# Automated gate (build + disassembly checks + all three runs + assertions)
tests/test-progs/egdiff_attack/run_layer5.sh
```

## 5. Caveats

- **Distances depend on codegen.**  `d_k1 = 5`, `d_k0 = 6` are properties of
  this reference implementation compiled with this GCC at `-O2`.  The
  intervening loads are the four 64-bit round constants loaded from the
  literal pool (RISC-V cannot encode them as immediates) and the `switch`
  jump-table load.  The gate's `objdump` checks and the end-to-end `MATCH`
  guard against silent layout drift, but a compiler change can move the
  distances; update `DIST_K1`/`DIST_K0` after re-running the disassembly.
- **The key line is warmed** immediately before the call (see §3.4), so the
  forward uses the base-available path.  The deferred `Wake` path also fires in
  the run (5531 times) but is not the path that carries the key.  If an
  in-flight-only demonstration is required, the warm-up can be removed and the
  attempt count raised, but the probe/squash race then makes individual
  attempts less reliable.
- `delayed_atk_load` must stay small.  An earlier 20-division version made GCC
  spill callee-saved registers and reload them between the key load and
  `L_atk`, which silently changed the base value (recovered bytes became
  stack-address fragments).  The gate's "0 loads in `delayed_atk_load`" check
  exists specifically to catch that class of regression.
- Single-core SE-mode experiment.  The architectural result is identical with
  the predictor off and with FLOP (`sink` matches), so no semantic state is
  corrupted; only the transient value is observed.
- One of the 80 byte-measurements (79/80 votes) missed the timing margin; the
  majority among the remaining attempts still yielded the correct byte.  The
  margin requirement is intentionally strict so a missed fill is an
  abstention, not a wrong vote.

> **Note (after the code-ownership split):** the target key and the helper
> moved to `egdiff_secret.c`, and the attacker translation unit now has no
> reference to the key.  The gate still passes (`LAYER 5 PASSED`, 24/24); the
> counts in this file (22/22, 79/80 votes, `sink=8976677841604280768`) are the
> pre-refactor numbers.  Current numbers: 24/24, 80/80 votes,
> `sink=8976677841604261728`.  Conclusions unchanged.
