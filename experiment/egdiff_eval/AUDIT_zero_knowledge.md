# Zero-knowledge audit — EgDiff cross-instruction value forwarding

*Auditor: independent subagent, no prior context. Everything below was derived
from the paper, the code in this tree, and my own runs. I did not read
`REVIEW_RESPONSE*.md`, `REVIEWS*.md`, commit messages, or report them as
evidence. I did not modify existing sources and did not commit. I did not run
the SafeSpec/DoM experiment.*

Environment used for the runs below:

- `build/RISCV/gem5.opt` as found (mtime 2026-11-24 23:18, 1.05 GB). **Not
  rebuilt.**
- `riscv64-linux-gnu-gcc`, `qemu-riscv64`, `nproc = 12`, 2 GHz tick
  (`simTicks/numCycles = 500.0`, verified on
  `/tmp/opencode/zk_layer4/poc_v1_specgvq_1shot.on/stats.txt`:
  6479239000 / 12958479).
- Fresh `WORK` dirs `/tmp/opencode/zk_layer*`, `/tmp/opencode/zk_out/*`.
- The tree carries an uncommitted `src/mem/cache/base.cc` hunk (pre-existing, not
  mine). It does not affect any number below: `git status` shows only that file
  modified, and all `src/cpu/**` files match `HEAD`.

---

## 0. TL;DR

| claim | verdict |
|---|---|
| **F1a** Paper allocates a GVQ slot for *all* candidates (§III-B) | **Supported** (quote verified; code allocates unconditionally *except* loads with no destination register — a documented deviation) |
| **F1b** Base is selected by *distance only*, with no base tag/PC (§III-B, Table I) | **Supported** (quote + Table I verified; code indexes `specGVQ[curIdx-d]` / `nvq[...]` with no tag compare) |
| **F1c** Algorithm 1 compares against the *queue entry's* distance/diff | **Supported** (§III-A text; code uses the dispatch snapshot) |
| **F1d** `lastMisp` = 1024-instruction global blackout after a value misprediction | **Supported with a caveat** (window implemented; it counts *dispatched*, not "correctly-dispatched", instructions — the header comment overstates it) |
| **F1e** Predictor latency +3 normal / +2 deferred is modelled | **Supported** (trace shows exactly 1500 and 1000 ticks), **but** the six gates and the SPEC runs all run at **0/0**; only `run_latency_sweep.sh` exercises it. It is not gate-enforced. |
| **Mechanism** Every load allocates, base by distance, attacker entry forwards an earlier load's value | **Supported** (traced: `base_val` = victim value) |
| **Mode 1** (in-flight SpecGVQ, crafted victim) 64-bit recovery, off/FLOP fail | **Supported** (17/17 gate; also 3/3 extra secret values) |
| **Mode 2** (NVQ capture + replay, fenced victim) | **Supported** but two of the write-up's numbers are stale (see §4.3) |
| **SipHash-2-4** 16/16, cold + warm | **Supported** (24/24 gate; my cold rebuild 16/16) |
| **A4 Monocypher `crypto_verify32`** 8/8 of `key[24..31]`, warm/cold/varying guess | **Supported, strongly** (36/36 gate; 3 variants 8/8; **new-key control recovers the new key exactly**) |
| **Observation** probe request precedes verification; fill survives squash | **Supported** for gem5 (margin reproduced exactly 54–185 cyc, avg 62; branch-squash control reproduced) |
| **F2 SPEC "smoke test"** | **Partially supported**: the *scope* statement is now honest, but the **numbers in the table are stale**; re-running at `HEAD` changes all five and the mean from 27.11 % to **26.84 %** (nab 28.38 %→27.48 %) |
| **A3 prevalence** | **Supported as a precondition measurement** (checked-in CSV is internally consistent); not re-run end-to-end |

**Bottom line.** The core security claim — an untagged, distance-selected global
value queue forwards a prior load's value into another instruction's register,
with no table collision and no victim gadget — is real in this model and is
demonstrated end-to-end. The A4 result is genuine (a changed key is recovered
correctly). The headline experimental numbers are sound but several are stale
relative to `HEAD`, one negative control (DIVS = 0) is **confounded**, and the
"architectural equivalence" checks in gates 5 and 6 are **tautologies**.

---

## 1. Paper fidelity

Page numbers: PDF p.1 = printed 574, so PDF p.5 = 578, p.6 = 579, p.7 = 580,
p.8 = 581, p.10 = 583.

### 1.1 Queue allocation for all candidates

> "Regardless of whether the prediction is valid or not, a new entry is allocated
> in the speculative queue for all candidates."
> — **§III-B, PDF p.6 (printed 579)**

Table I (PDF p.7 / printed 580) confirms this: entries 2, 5 and 6 have `VP = 0`
but are present, i.e. presence is not gated by a valid prediction.

Code: `src/cpu/lvp/egdiff_predictor.cc`

- `dispatchLoad()` allocates **unconditionally** at the end
  (`specGVQ.push_back(q); res.allocated = true;` — **lines 212–214**). The only
  gates are duplicate-seqNum idempotence (134–137) and a 32-entry cap
  (141–142). There is no tag/confidence/EVES gate on allocation.
- Call site: `src/cpu/o3/iew.cc:1216–1233` — called for every load **with
  `inst->numDestRegs() > 0`**.

**Deviation (disclosed, scope-honest):** loads with no destination register are
excluded from both allocation and commit training
(`iew.cc:1222`, `commit.cc:1030`). The paper says *all* candidates. In these
PoCs and on RISC-V gem5 (no prefetch instructions generated, no prefetcher
enabled) this changes nothing, but it *is* a distance-space change in general:
`d` is measured over *tracked* loads, not all loads. The submission discloses
this (limitation 18). Honest.

### 1.2 Base by distance, no tag

> "The speculative queue is then accessed using the distance info to retrieve
> the base value." — **§III-B, PDF p.6 (printed 579)**

Table I (p.580) columns are `Index | Value | Distance | Diff | VP | Misp | Avail.
| Wake` — **no base PC and no base tag**.

Code: `egdiff_predictor.cc`

- Speculative base: `const EgDiffSpecEntry &base = specGVQ[curIdx - d];`
  with **no** tag or PC comparison (lines 173–192).
- Committed base: `baseVal = nvq[d - curIdx - 1];` (lines 193–200). The NVQ is
  `std::array<uint64_t, ORDER_N>` (`egdiff_predictor.hh:221`) — values only, no
  PC. `egdiffNVQBases` records that this path was taken.
- The stored `res.basePc` (line 184/99) is for tracing only; it is never
  compared.

**Fidelity note (new, medium):** the paper says *"The non-speculative segment is
responsible for updating the global predictor using committed values, whereas
the speculative part is used for performing prediction"* (§III, PDF p.5/578) and
describes only the speculative queue as the prediction-time base. The code adds
a **committed-base (NVQ) read at dispatch** (§1.2 above). This is not a quoted
part of the paper's prediction workflow. It matters for impact:

- I traced A4 (`--debug-flags=EgDiff`, TRAIN=128/REPS=2): every victim-phase
  marker prediction is `base=nvq, base_pc=0` with `base_val` equal to a key byte
  (`0x6a, 0x4c, 0xbd, 0x2e, …`). **No** key load ever appears as a speculative
  `base_pc`.
- The cold-key A4 variant behaves the same (also `base=nvq`, `base_val =
  0xa1/0x10/0x8f/0xdb`).
- SipHash (REPS=2 debug run): the marker has 32 `base=nvq` predictions and 9
  `base=specgvq` ones; every `specgvq` base PC lies in the *training* functions,
  so the victim-phase forwards are all NVQ-sourced.

So **both real-target demonstrations ride entirely on the committed-NVQ base
path**. This is defensible — the queue is a circular buffer allocated in
lock-step with the LQ, and a just-committed slot is still addressable by
distance, so a paper-faithful implementation may leak the same way — but the
submission should say that the quoted paper rules do *not* cover this path, and
that without it the two real-target results would not exist. The crafted
SpecGVQ path (Mode 1, Mode 2 replay) is paper-faithful and does the mechanism
work.

### 1.3 Algorithm 1 uses the queue entry's distance/diff

> "the predictor compares the actual stride with the predicted global stride
> stored in the value queue entry" — **§III-A, PDF p.5–6 (printed 578–579)**

Code: `egdiff_predictor.cc:41–45` finds the entry's dispatch snapshot; lines
**61–64** use `qit->distance` / `qit->diff` rather than the live table entry;
lines 69–81 implement the poll (`d→d-1`, `δ←v−NVQ[d-1]`, wrap to `n` at 1);
lines 82–92 are the match branch (FPC + u). Correct, and it is the *right*
reading of the paper.

### 1.4 `lastMisp` 1024-instruction window

> "After a misprediction occurs, the predictor temporarily suppresses all
> prediction outputs over a predefined window of 1024 instructions."
> — **§III-A, PDF p.5 (printed 578)**

Code: `LAST_MISP_WINDOW = 1024` (`egdiff_predictor.hh:120`); suppression check
`instructionCount <= suppressUntilCount` (198–203); the window is armed at the
squash (`suppressUntilCount = instructionCount + 1024`, cc:345–348); the
counter is incremented at dispatch (`iew.cc:1163–1166`) for **every**
instruction.

**Discrepancy (new, low):** the header says

> "Count every instruction dispatched on the correct path. … squashed wrong-path
> instructions must not consume it." (`egdiff_predictor.hh:194–197`)

Dispatch happens *before* execute, so wrong-path instructions that were
renamed/dispatched before the redirect **do** increment the counter. The
submission's Appendix C claim "[R2] `lastMisp` counts correctly-dispatched
instructions" is therefore precise only in the weak sense that the window starts
at the squash. The error is a few instructions per mispredict (negligible in the
PoCs, small in SPEC). Honest fix: count at commit, or reword.

### 1.5 Predictor latency +3 / +2

> "EgDiff includes two prediction modes: a normal prediction with 3-cycle
> latency, and a deferred prediction with 2 additional cycles."
> — **§IV, PDF p.8 (printed 581)** (immediately above TABLE II; the latency is
> *not* a row of Table II).

Code: `LoadValuePredictionUnit.py:88–107` (`egdiffLatency`,
`egdiffDeferredLatency`, default **0**), `configs/example/egdiff.py:31–32`
(default 0), `iew.cc:508–600` (pending-publish stage), `inst_queue.cc:1558`
(destination stays not-ready while pending).

**Independently verified trace** (`test_layer3_deferred_wake`, LATENCY=3,
DEFERRED_LATENCY=2):

```
schedule-publish ... sn:15369 val ... at 13236500 (+3)   ; tick delta = 1500 = 3 cyc
publish ... sn:15369 val ...     @ 13236500
schedule-publish ... sn:59426 ... at 47772000 (+2 deferred)  ; delta = 1000 = 2 cyc
deferred prediction published ... sn:59426 @ 47772000
```

**Scope caveat:** none of `run_layer1..6.sh` or `run_spec.py` sets
`EGDIFF_LATENCY`; only `run_latency_sweep.sh` / `run_probe_delay.sh` do. The 117
conformance checks, the A4/SipHash wins, and F2 all execute at **0/0**. The
submission says the latency "is modelled" (true) and reports a separate matrix
(true), but a reader can easily infer that the headline runs include it. They do
not.

---

## 2. Attack mechanism — exact code paths

| step | code |
|---|---|
| every load allocates | `iew.cc:1216–1233` → `egdiff_predictor.cc:212` |
| attacker's entry trained to `diff=0,d` | attacker PoC training loop → commit `egdiff_predictor.cc:57–92` |
| prediction gated only by tag + `ctr==7` | `egdiff_predictor.cc:144–166` |
| base = queue position `curIdx-d`, **no tag** | `egdiff_predictor.cc:173–192` |
| or base = NVQ `d-curIdx`, **no PC** | `egdiff_predictor.cc:193–200` |
| `pred = baseVal + diff` | `egdiff_predictor.cc:202–208` |
| value written to PRF / wake | `iew.cc:508–600` |
| verify at LQ completion, squash on mismatch | `lsq_unit.cc:1294–1322`, `egdiff_predictor.cc:233–249` |
| 1024-instruction blackout | `egdiff_predictor.cc:244–247,343–348` |

Yes: **every** tracked load allocates a slot, the base is chosen by distance
alone, and an attacker-trained `diff=0` entry forwards the base load's value. I
confirmed the identity of the base in the trace: for A4 the admitted base value
is the key byte (`base_val=0x6a …`), not a guess or a training value.

**New-key control (mine).** I copied `target_verify32.c` to `/tmp`, changed the
last key word to `0x88 77 66 55 44 33 22 11`, rebuilt the A4 harness and ran it
with the same lane map. Result:

```
verify32_attack key_word=8877665544332211 recovered=8877665544332211 ... 8/8 MATCH
```

This is the strongest single piece of evidence that the recovered word tracks
the *victim's* secret, not a harness constant.

**No-victim control (mine, new).** I stubbed `target_verify32` to return 0
without loading the key, keeping everything else:

```
verify32_attack key_word=8877665544332211 recovered=0000000000000000 known=ff 8/8 NO-MATCH
```

The estimator still reports `known=ff` (all 8 lanes "measured") and a confident
all-zero word. So `known=ff` certifies *"a forwarded value was measured"*, **not**
*"the victim's value was measured"*. The 8/8 A4 result is saved only by the
oracle comparison (and by my key-change test). This should be stated; the
submission's "the harness never claims a byte it did not measure" is true but
weaker than it reads.

---

## 3. Independent test results

### 3.1 Gates (fresh WORK dirs)

```
run_layer1.sh   17 passed, 0 failed   LAYER 1 PASSED   (/tmp/opencode/zk_layer1.log)
run_layer2.sh   10 passed, 0 failed   LAYER 2 PASSED   (/tmp/opencode/zk_layer2.log)
run_layer3.sh   13 passed, 0 failed   LAYER 3 PASSED   (/tmp/opencode/zk_layer3.log)
run_layer4.sh   17 passed, 0 failed   LAYER 4 PASSED   (/tmp/opencode/zk_layer4.log)
run_layer5.sh   24 passed, 0 failed   LAYER 5 PASSED   (/tmp/opencode/zk_layer5.log)
run_layer6.sh   36 passed, 0 failed   LAYER 6 PASSED   (/tmp/opencode/zk_layer6.log)
total 117
```

Layer 3 numbers: deferred bindings 437, deferred wakes fired 235, ignored 202,
normal predictions 13 929, NVQ bases 12 914, correct 14 154, incorrect 0.

**Gate-count inconsistency:** §4's table says layer 4 = **17** (observed);
Appendix A's command block says `# 14`. Same script, two numbers.

**Tautological checks:** layer 5 and layer 6 each count one PASS for "identical
architectural checksum `sink`". In both harnesses `sink` is the training-loop
sum plus reads of `probe[]`, which is `memset(0)` and never written, so `sink`
cannot depend on the recovered value and is identical by construction. The
submission discloses this for A4 but still counts them in the 117. The real
architectural check is `arch=OK` / `archpre self/repro/marker=OK`.

### 3.2 Latency matrix (`poc_v1_div12`)

| normal | deferred | result |
|---|---|---|
| 0 | 0 | `MATCH known=ff` |
| 3 | 0 | `MATCH known=ff` |
| 0 | 2 | `MATCH known=ff` |
| 3 | 2 | `MATCH known=ff` |
| off (`EGDIFF_ON=0`) at 3/2 | — | `NO-MATCH known=00` |

So the matrix is reproduced. Trace: +3 and +2 confirmed (§1.5).

### 3.3 Mode 2 controls (layer 4)

```
[on]   secret=1122334455667788 recovered=1122334455667788 known=ff  arch=OK MATCH
[off]  recovered=0000000000000000 known=00 arch=OK NO-MATCH
[flop] recovered=0000000000000000 known=ff arch=OK NO-MATCH
```

The off control is **`known=00`** (the `flush_drain` fix works). FLOP emits its
own 8 zero bytes (`known=ff`). Gate: `mode 2: architectural precondition holds`,
`mode 2: committed-NVQ base predictions occurred`.

The Mode 2 fence claim is accurate at the ISA level: in this gem5,
`fence_i` is decoded with `IsNonSpeculative, IsSerializeAfter, IsSquashAfter`
(`src/arch/riscv/isa/decoder.isa:1340–1341`), i.e. it does serialise the pipeline
(all older instructions must commit first), so `flush_drain()` really does drain
the eviction's MSHRs — but on RISC-V it orders *instruction fetch*, not data, as
the submission says. Mode 2's correctness therefore rests on the harness's
`fence.i`, which is an ordering device, not a data-fence claim.

### 3.4 Real-target controls

| run | result |
|---|---|
| A4 warm (baseline, layer 6) | `8/8 MATCH`, key word `a1108f2ebd4cdb6a`, min lane votes 16 |
| A4 `COLD_KEY=1` | `8/8 MATCH` |
| A4 `VARY_GUESS=1` | `8/8 MATCH` |
| A4 `COLD_KEY=1 VARY_GUESS=1` | `8/8 MATCH` |
| A4 **new key** (mine) | `8/8 MATCH`, `8877665544332211` |
| A4 **no victim** (mine) | `known=ff`, `0x0…0`, NO-MATCH |
| SipHash warm (layer 5) | `16/16 MATCH`, votes 80/80 |
| SipHash `COLD_KEY=1` (mine) | `16/16 MATCH`, votes 80/80 |

Measured A4 map (reproduced by `measure_d.py` in layer 6):
`24:13, 25:14, 26:12, 27:11, 28:10, 29:9, 30:8, 31:7`, `d_min=7, d_max=14`.

### 3.5 Mode 1 address-delay sweep

| divisions | result |
|---|---|
| 0 | `NO-MATCH known=00` (see §4.2 — **confounded**) |
| 1 | `MATCH known=ff` |
| 12 | `MATCH known=ff` |

### 3.6 Race margin (fresh `--debug-flags=EgDiff` run, `poc_v1_div1`)

`measure_race_margin.py --probe-base 0x98300 --probe-size 0x4000`, 126 matched
rows:

```
pub->request     min  5.0  max 18.0  mean 11.8 cycles
verify->request  min 54.0  max 185.0 mean 62.1 cycles
request->squash  min 54.0  max 185.0 mean 62.1 cycles
```

This **exactly reproduces** the claimed 54–185 / avg 62 at 1 division. Tick→cycle
conversion checked: `simTicks/numCycles = 500`. The probe's cache request is
issued ~62 cycles before verification, i.e. the transient window is real in the
model.

### 3.7 Branch-squash survival (`run_branch_squash.sh`, `EGDIFF_ON=0`)

```
arch   : hot=16 cold=146  sink=0
squash : hot=16 cold=114  sink=170      <-- bad path squashed, line still hot
none   : hot=116 cold=114 sink=8330     <-- null: line stays cold
```

Confirms the fill survives a branch-misprediction squash and is not specific to
`IEW::squashDueToMemOrder`.

### 3.8 F2 SPEC — re-run at `HEAD`

| bench | committed loads | CSV coverage | **my coverage** | CSV acc | my acc |
|---|---|---|---|---|---|
| 505.mcf | 6,426,344 | 64.51 % | **64.48 %** | 99.99 % | 99.99 % |
| 544.nab | 5,527,140 | 28.38 % | **27.48 %** | 99.60 % | 99.60 % |
| 548.exchange2 | 4,951,309 | 17.52 % | **17.35 %** | 99.49 % | 99.52 % |
| 510.parest | 5,161,173 | 22.84 % | **22.70 %** | 99.83 % | 99.83 % |
| 531.deepsjeng | 22,357 | 2.29 % | **2.20 %** | 95.35 % | 95.16 % |
| mean (5) | | 27.11 % | **26.84 %** | 98.85 % | 98.82 % |

Committed-load counts are byte-identical; only the predictor numerator differs.
The run is deterministic (mcf reproduced 64.482 % twice). The CSV dates from
commit `2eeb193ea8` (15:46), *before* the three predictor changes at 22:37–23:21
(`c301573861`, `6e6fcd4d8d`, `5a81c3ed1b`, `0c406768a3`). So the checked-in
table and `SUBMISSION.md` §7.1 / Appendix B are **stale by 0.03–0.90 pp**. The
qualitative conclusion is unchanged.

### 3.9 F2 is a startup window

`--maxinsts=30000000` sets `cpu.max_insts_any_thread`
(`configs/common/Simulation.py:507–509`), i.e. the first 30 M *committed*
instructions from `_start`, no warm-up, no SimPoints. Confirmed. The paper's
Fig. 10(b) (PDF p.10 / printed 583) does show `505.mcf` coverage at ≈5 %, so the
"mcf 64.5 % is a startup artifact" statement is **supported by the figure**.

Residual band claim: `spec_coverage/README.md` still ends with "coverage in the
expected band", and `run_spec.py` says "roughly 22-26 % coverage"; §7.1 of the
submission correctly forbids the comparison. Internal inconsistency (low).

---

## 4. Discrepancies and new issues

### 4.1 A4/SipHash depend on an *unspecified* NVQ-at-dispatch path — **medium**

See §1.2. All victim-phase base reads in both real-target harnesses are
`base=nvq` / `base_pc=0`. The paper's quoted prediction path uses the
*speculative* queue ("the speculative part is used for performing prediction").
The code's committed-base fallback (`egdiff_predictor.cc:193–200`) is not quoted
or described. Consequence: the "real targets" results should be labelled
"forwarding from the **committed** (NVQ) side of the queue", and the paper's
design as literally written (speculative queue only) is not what was attacked
there. Mode 1 and Mode 2 *do* exercise the paper-faithful SpecGVQ path.

### 4.2 The `DIVS=0` "race is real" control is confounded — **medium**

`poc_v1` sets only `atk_buf[0] = SENTINEL`; all other entries are 0. At
`ATK_DIVS=0`, `x = (ok|1) = 1`, so the marker architecturally reads
`atk_buf[1] = 0`, not the sentinel. The estimator's calibration line is
`lat[arch]` where `arch` is the **sentinel byte**, which is therefore never
filled; every attempt is discarded as `invalid`:

```
byte 0: UNKNOWN votes=0 sentinel_only=0 ambiguous=0 invalid=16
```

for all 8 bytes. So `NO-MATCH` at 0 divisions says "the harness's sentinel
reference was absent", not "the race was lost". The claim that "the attack fails
at 0 divisions" is therefore **not** supported by this control. (The conclusion
is still supported by the 54–185-cycle margin, which I reproduced.) A clean
control would force `x==0` at 0 divisions or drop the architectural sentinel
requirement.

### 4.3 Mode 2 `§7.3` row contradicts §5 and the code — **low/medium**

§7.3 says Mode 2 off "NO-MATCH but emits **7 wrong bytes** (`3f3f…00`,
`known=fe`)". §5 says the post-`flush_drain` off control is **`known=00`**. My
run (and the gate) give `known=00`. The §7.3 row is stale; §5 is right.

Also stale: §7.3/§9.11 quote `numPredictionIncorrect = 369` (A4) and 506
(SipHash); at `HEAD` I measure **355** and **472**.

### 4.4 F2 table is stale — **low/medium**

See §3.8. Appendix B still presents `27.11 %`.

### 4.5 Two of the 117 checks are tautologies — **low**

"sink equality" in layers 5 and 6 cannot fail: `probe[]` is never written and
`sink` does not include the recovered value. The submission admits this for A4
but not for layer 5, and still counts both. Effective check count ≤ 115.

### 4.6 `known=ff` does not certify provenance — **medium**

Demonstrated with the no-victim control (§2): all 8 lanes "measured" a confident
`0x00`. So `known` is a confidence mask over *some* forwarded value, and A4's
result rests on the oracle comparison. This is fine, but the wording "never
claims a byte it did not measure" should be "never claims a byte for which no
single line won the timed scan".

Measurements are also only taken to a 1/256 line-resolution and 2× margin; the
attacker therefore learns a *byte*, not a timing-independent value.

### 4.7 Gate 2's blackout check uses sequence numbers, not instructions — **low**

`run_layer2.sh` verifies "no value forwarded during the 1024-inst blackout" with
`sn > t && sn <= t+1024`. Sequence numbers increment per fetched instruction and
diverge from the instruction-count window whenever wrong-path instructions
consume seqNums (they do). The implemented window is still 1024 *dispatched*
instructions; the gate tests a proxy, not the quantity in the paper.

### 4.8 "1-shot" is per attempt, not per extraction — **low**

`poc_v1` uses `REPS=16` attempts/byte and `TRAIN=400` iterations **before every
attempt**. §5's "call the victim once; execute the attacker's load immediately"
describes one attempt. A full 64-bit extraction costs 128 victim calls and 128
squashes/blackouts. The submission's "each leaked byte costs a squash and a
1024-instruction blackout" is the honest version; "1-shot" in the filename is
not.

### 4.9 Harness fragility/other observations — **low**

- `poc_v1`'s estimator needs the architectural re-execution to land on the
  sentinel line; any change to the architectural path (as at DIVS=0) silently
  invalidates every attempt. It is not a general measurement tool.
- `measure_d.py` uses the *architectural* (qemu) dynamic order as a proxy for the
  *dispatch* order and for the GVQ's tracked-load order. Valid only in
  straight-line, all-loads-have-dest windows; the submission says so.
- `probe[]` is only ever zero; the `g_sink` accumulator is therefore not an
  architectural checksum of leaked data (disclosed for A4, not for layers 5/6).
- The model's subword extension gap is real: an `lb`/`lw` prediction is written
  un-extended, so verification will mismatch on every signed narrow load
  (squash + blackout). Disclosed (limitation 18), but it means the model would
  mispredict badly on ordinary code and should not be read as a performance
  model.
- The `d` map is codegen-specific; A4 re-measures it per build (good), but the
  attack does not discover `d` at runtime (disclosed).
- `specGVQ` is a capped 32-entry deque; a *literal* paper implementation would be
  a 32-slot circular buffer indexed by the LQ. The value-flow is equivalent for
  the demonstrated cases; the NVQ fallback supplies the "base has left the
  speculative window" case.
- `lastMisp` counting of wrong-path instructions (§1.4).
- `tagHash == 0` on a cold entry makes `e.tag == t` true and lets `commitLoad`
  run Algorithm 1 on a never-allocated entry (it can accrue `ctr`/`u` if the
  load's value equals `NVQ[32] == 0`). Cosmetic at 1/16384.

---

## 5. What the artifact can and cannot be said to demonstrate

**Can be said:**

1. A faithful-in-the-essential-parts gem5 model of a global, distance-indexed,
   **untagged** value queue forwards the value of an earlier dynamic load into a
   later load's destination register.
2. On crafted victims this forwarding is a *register-level* leak: the leaked
   value drives a dependent cache access that fills a line, and the fill
   survives the value-misprediction squash (and, separately, a branch squash).
   At 1 division the probe request precedes verification by 54–185 cycles
   (mean 62) on this configuration.
3. On two real library paths — Monocypher `crypto_verify32`'s last key word
   (d = 7…14) and SipHash-2-4 with an empty message (d = 5, 6) — the forward is
   the victim's key material: a changed key is recovered as the changed key, and
   warm/cold key-line and varying-guess variants all recover.
4. Clean negative controls: predictor off and a FLOP local predictor recover
   nothing; the 1024-instruction blackout is observed; the gates pass 117/117.
5. The +3/+2 latency model does not break the attack (all four matrix points),
   and the latency is visible in the trace.

**Cannot be said (or is not yet supported):**

1. That this reproduces EgDiff's *performance* or its steady-state behaviour.
   F2 is a 30 M-instruction startup smoke test at 0-cycle latency, with a
   different branch predictor, register file and no L3/prefetcher; and its table
   is stale.
2. That the real-target leaks use the paper's *speculative*-queue prediction
   path. They use the committed NVQ, a path the paper does not describe for
   prediction.
3. That the DIVS=0 control shows the race is necessary — it does not (the
   estimator's calibration line disappears).
4. That the attack is practical or stealthy: it needs offline knowledge of `d`,
   a re-train per byte, ~10² victim calls per word, and emits a squash plus a
   1024-instruction prediction blackout per leaked byte.
5. That the leak survives speculative-cache defences (SafeSpec/DoM untested), or
   crosses any privilege/process boundary (explicitly a same-process
   non-disclosure convention on an undefended cache model).
6. That the "architectural equivalence" checks prove architectural equivalence
   (they are tautologies; `arch=OK` is the real check).
7. Any A3 prevalence *attack*: the prevalence study measures a precondition,
   on one real library plus hand-written controls, with an over-approximating
   fault probe and a min-over-PCs statistic. (I checked the 6,300-row CSV for
   internal consistency — `base_pc == d_pc`, `d_key == d`, `loads_since_ret ==
   0` in all rows, distributions as tabulated — but did not re-run the host
   measurement.)

---

## Appendix — commands used (all reproducible)

```bash
# gates
WORK=/tmp/opencode/zk_layer1 bash tests/test-progs/egdiff_attack/run_layer1.sh
#   … layer2 … layer6 (same pattern)

# latency matrix
riscv64-linux-gnu-gcc -static -O2 -fno-pic -no-pie -DATK_DIVS=12 \
  -o /tmp/poc_v1_div12 tests/test-progs/egdiff_attack/poc_v1_specgvq_1shot.c \
  tests/test-progs/egdiff_attack/egdiff_secret.c
for cfg in "0 0" "3 0" "0 2" "3 2"; do set -- $cfg
  EGDIFF_TYPE=egdiff EGDIFF_ON=1 EGDIFF_LATENCY=$1 EGDIFF_DEFERRED_LATENCY=$2 \
  build/RISCV/gem5.opt -d /tmp/lat_$1_$2 configs/example/egdiff.py \
    --cpu-type=O3CPU --caches --cmd=/tmp/poc_v1_div12
done

# A4 controls
MONO=experiment/egdiff_eval/prevalence/third_party/Monocypher/src
for cfg in "0 0" "1 0" "0 1" "1 1"; do set -- $cfg
  riscv64-linux-gnu-gcc -static -O2 -fno-pic -no-pie -fno-ipa-icf -I"$MONO" \
    -DCOLD_KEY=$1 -DVARY_GUESS=$2 -o /tmp/a4_$1$2 \
    tests/test-progs/egdiff_attack/poc_verify32.c \
    tests/test-progs/egdiff_attack/target_verify32.c "$MONO/monocypher.c"
  # run with --options="attack 24:13,25:14,26:12,27:11,28:10,29:9,30:8,31:7"
done

# race margin (1 GB trace, deleted afterwards)
EGDIFF_TYPE=egdiff EGDIFF_ON=1 build/RISCV/gem5.opt -d /tmp/race \
  --debug-flags=EgDiff --debug-file=/tmp/race/simout configs/example/egdiff.py \
  --cpu-type=O3CPU --caches --cmd=/tmp/poc_v1_div1
python3 tests/test-progs/egdiff_attack/measure_race_margin.py \
  --log /tmp/race/simout --probe-base 0x98300 --probe-size 0x4000

# F2
python3 experiment/egdiff_eval/spec_coverage/run_spec.py --maxinsts 30000000 \
  --benches 505.mcf,544.nab,548.exchange2,510.parest,531.deepsjeng \
  --work /tmp/opencode/zk_spec
```

Artifacts left in `/tmp/opencode/`: `zk_layer{1..6}.log`, `zk_out/*`,
`zk_spec*/spec_f2.csv`, `zk_var/` (patched copies of the A4 target used for the
new-key and no-victim controls), `zk_bins/`. All large traces were deleted.
No repository file was modified; the test binaries produced by the gate scripts
are git-ignored build outputs.

---

## Post-audit addendum (authors)

After the audit, we added `egdiffNvqBase`, default on, and reran the real targets
with it **off** (`EGDIFF_NVQ_BASE=0`). This tests the paper-faithful
speculative-queue path only.

- **A4:** still recovers 8/8 at 1, 4, and 12 divisions, with
  `egdiffNVQBases=0`, `base=specgvq`, and nonzero `base_pc` (e.g. `0x10cce`).
  The strict Layer-6 gate passes 36/36. The 2-division run is a measurement
  false negative. The audit's “every base NVQ” observation is correct for the
  default model with the extension on, but A4 has a paper-faithful SpecGVQ
  result when it is off.
- **SipHash:** strict SpecGVQ fails 0/16 at 1, 2, 4, and 12 divisions, and
  0/16 with `COLD_KEY=1`. Its 16/16 result is therefore an NVQ-extension
  result, not a paper-faithful SpecGVQ one.
- **Crafted Mode 1:** strict SpecGVQ 8/8 at 1 and 12 divisions, `egdiffNVQBases=0`.
- **Mode 2:** unchanged; it uses the paper's NVQ update/capture path.

The paper draft and submission now classify A4 as paper-faithful SpecGVQ and
SipHash as an NVQ-extension result. The A4 gate defaults to
`EGDIFF_NVQ_BASE=0`; Layer 5 explicitly sets it to 1.
