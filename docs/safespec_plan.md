# SafeSpec implementation plan — layered, test-driven

This plan builds SafeSpec in layers. Each layer has a **pre-registered goal** and
**pre-registered acceptance tests**; the tests are written and shown to fail
before the layer is implemented. No layer is built on an unverified one.

The plan is derived from Khasawneh et al., *SafeSpec: Banishing the Spectre of a
Meltdown with Leakage-Free Speculation* (arXiv:1806.05179v2), and is shaped by
the gaps found in the current L1-D prototype.

---

## 0. Rules of the process

### 0.1 Test-first protocol

Every layer follows the same six steps. A layer is not "done" until all six are
recorded.

1. **Pre-register the goal.** One falsifiable sentence plus the invariants it
   implies.
2. **Pre-register the tests.** Names, inputs, expected results, and the reason
   each test could fail for the *right* reason.
3. **Red run.** Run the tests against the pre-layer code. The failure must be
   the expected one; a test that fails for an unrelated reason is not evidence.
4. **Implement.**
5. **Green run.** All new tests pass and all previous layers' tests still pass.
6. **Mutation check.** Deliberately break the implementation in one or two
   plausible ways; the tests must catch it. If they do not, the test is
   incomplete.

### 0.2 Test taxonomy

| id | kind | question it answers |
|---|---|---|
| U | unit | does the structure obey its own invariants? |
| D | directed microbenchmark | does the mechanism do exactly one thing correctly? |
| E | differential / equivalence | is architectural output byte-identical to baseline? |
| S | security | does the attack leak without the mechanism and not with it? |
| C | conservation / stats | do the counters account for every entry? |
| P | performance | what does it cost over an identical instruction window? |
| M | mutation | would the tests notice a plausible bug? |

Each layer lists which categories it requires. A layer that only has E is not
verified: output equivalence does not prove the side channel is closed, and a
security test does not prove the data path is correct.

### 0.3 Definition of done

A layer is done only when:

- every pre-registered test has a red run and a green run recorded;
- the mutation check catches both injected bugs;
- all earlier layers' tests still pass (regression gate);
- the layer's invariants are either asserted in code or checked by a test;
- the known coverage gaps are written down ("what this layer does **not**
  prove").

---

## 1. Layer map

| layer | scope | status in current tree |
|---|---|---|
| 0 | shadow buffer: ownership, lifetime, overflow | implemented, 29 unit tests |
| 1 | L1-D capture + wait-for-commit promotion | implemented, 5/5 equivalence, 2 attacks |
| 2 | write / atomic / split / SMT correctness | mostly done: writes/atomics/LL-SC, split, per-thread release; secure forbidden; full SMT and snoop open |
| 3 | shadow-hit serve + request merge | implemented: serve, safe/unsafe split, serve gate, directed harness; 4-cycle policy |
| 4 | cache hierarchy (L2/L3) | not started; only valid today because SE has no L2 |
| 5 | instruction cache + TLB shadows | not started |
| 6 | coherence / multicore / TSO | out of scope in the paper; needed for full-system |
| 7 | transient-speculation (TSA) hardening | partially addressed by overflow-refusal |
| 8 | performance and representativeness | measured at 200 M / 1 B windows |

Layers 0–2 are prerequisites for 3–5. Layer 4 depends on 3 for the request
path. Layer 6 depends on 4. Layer 7 can be done in parallel after 2.

---

## Layer 0 — Shadow buffer (structure only, no simulator)

**Goal.** A standalone structure that holds speculative lines with a single,
testable owner and a bounded lifetime, independent of gem5.

**Invariants (pre-registered).**

- I0.1 One entry per line; a second capture of the same line refreshes contents
  and keeps the original owner.
- I0.2 The owner is the oldest captured, not-yet-retired load.
- I0.3 Entries are ordered by owner sequence number.
- I0.4 Overflow is refused; no entry is evicted to make room.
- I0.5 `retireThrough(w)` removes entries with owner `< w`.
- I0.6 `discardFrom(s)` removes entries with owner `>= s`.
- I0.7 No sequence of operations can retain an entry forever.
- I0.8 A refused capture is distinguishable from an accepted one.

**Pre-registered tests.** 29 GTests in `src/mem/cache/shadow_buffer.test.cc`,
covering: capture/refresh, ownership, sorting, overflow refusal, promotion,
sweep direction, squash direction, watermark monotonicity, mixed workloads.

**Evidence.** `./build/RISCV/mem/cache/shadow_buffer.test.opt` → 29/29.

**Mutation check.** Change the owner rule to "newest" → tests must fail. Change
the sweep to `>` instead of `>=` → tests must fail. (Already performed for the
first implementation.)

**Coverage gap.** The structure knows nothing about time, so it cannot prove the
caller supplies a correct watermark. That is Layer 1's job.

---

## Layer 1 — L1-D capture and wait-for-commit promotion

**Goal.** A speculative load that misses in L1-D must not install its line in
the L1-D before it commits; the load still receives correct data; a squashed
load leaves no line anywhere in L1-D.

**Invariants.**

- I1.1 Between fill and commit, the line is absent from the L1-D tag array.
- I1.2 The load receives byte-correct data even though the line is not
  installed (the diverted fill still serves the MSHR target).
- I1.3 At commit, the line is installed exactly once.
- I1.4 On squash, the line is never installed.
- I1.5 The retirement watermark reclaims entries whose owner will never commit
  (late fills, refused captures).
- I1.6 A line with a concurrent store is not silently left unprotected (this is
  the known gap; see Layer 2).

**Pre-registered tests.**

| test | kind | red condition |
|---|---|---|
| `promote_visibility` — one speculative miss; probe tags before commit | D | line is resident before commit |
| `squash_invisible` — miss then squash; probe the line | D/S | line is resident |
| `commit_installs` — miss then commit; probe | D | line is absent after commit |
| `late_fill` — miss, squash, fill arrives after | D | entry stranded / line never installs |
| 5 output-equivalence programs | E | any output differs |
| `spectre_v1`, `flop_attack` | S | secret recovered with mechanism on |
| captures = promotions + squashes + drops + occupancy + declines | C | counters do not close |

**Evidence.** Harness 5/5; both attacks red without / green with; conservation
of the shadow counters over a 1 B run.

**Exit criteria.** All of the above, plus a mutation check on the squash hook
(call `retireThrough` instead of `discardFrom` → `squash_invisible` must fail).

**Coverage gap.** Single-core SE, no L2, no SMT, no coherence. Layer 1 proves
the L1-D tag array, nothing below it.

---

## Layer 2 — Writes, atomics, split loads, SMT

**Goal.** Every architectural event that can modify or observe a promoted line
leaves the shadow and the cache in a coherent, non-stale state.

**Pre-registered failures to cover (each is a test).**

- I2.1 A store to a line held in the shadow must not be lost; promotion must
  not install pre-store data.
- I2.2 An AMO (`SwapReq`, atomic op) to a promoted line must not leave a dirty
  block marked as silently droppable.
- I2.3 An LL/SC write to a promoted line must not trip the dirty-line invariant
  or livelock the SC.
- I2.4 A split (unaligned) load must install **both** lines, not one.
- I2.5 A promoted line must carry the correct secure bit (or the config must
  forbid secure memory).
- I2.6 SMT: one thread's watermark/squash bound must not delete the other
  thread's live entries.
- I2.7 A writeback from below must not be announced for a line the cache never
  announced (holder-bit consistency).

**Pre-registered tests.**

| test | kind |
|---|---|
| `dirty_promote`: store to a held line, then commit the load | D |
| `amo_promote`: swap/atomic on a promoted line | D |
| `llsc_promote`: LR/SC pair on a promoted line | D |
| `split_promote`: unaligned load spanning two lines | D |
| `secure_promote` (if secure memory is enabled) | D |
| SMT two-thread watermark test | D |
| snoop/invalidate while held (requires Layer 6) | D |
| output equivalence including all of the above | E |
| `spaceSpecDirtyPromoted == 0` on a clean run | C |

**Current status (frontier).** `setDirty()` and the later write/atomic/LL-SC
drop hook cover I2.1–I2.3; split loads are covered by `promoteAll()` (I2.4);
per-thread release is covered by the thread tag and the interleaved unit tests
(I2.6 at the structure level).  Secure memory (I2.5) is **forbidden**: the
shadow key has no secure bit, so a secure speculative fill is a fatal
configuration error rather than a silently unprotected line.  Snoop/invalidate
(I2.7) is Layer 6.  A full SMT run is still a coverage gap; the per-thread
contract is unit-tested, not simulated with two hardware threads.

**Exit criteria.** Every test above has a red run on the pre-fix code and a green
run after. The audit's four correctness bugs are the seed tests.

---

## Layer 3 — Shadow-hit serve and request merge

**Goal.** A load that touches a line already held in the shadow is satisfied
from the shadow, not by a second memory request; concurrent loads to the same
held line share one request.

**Invariants.**

- I3.1 A shadow hit returns byte-identical data to a normal fill.
- I3.2 A shadow hit does not consume or re-key the entry.
- I3.3 A shadow hit does not allocate an MSHR.
- I3.4 Concurrent misses to a held line result in one memory request.
- I3.5 The hit latency is a deliberate policy, not an accident: either the miss
  latency or a fixed latency, and the choice is documented (it is a timing
  channel candidate).

**Pre-registered tests.**

| test | kind |
|---|---|
| `shadow_hit`: speculative miss, second load to same line; compare data | D |
| `hit_no_mshr`: check `mshr` counters / `lookupHits` | C |
| `merge`: N concurrent loads to a held line → 1 fill | C |
| `hit_latency`: measure hit vs miss latency; assert the policy | D |
| output equivalence | E |
| refresh count drops vs Layer 1 | P |

**Instrumentation prerequisite (done).** The held-line miss counter classifies
safe (same thread, younger) versus unsafe (older or another thread); the
pressure harness reported 2.8%.  `lookupHits` now counts serves through
`lineForConsumer`, not the unused generic `lookup()`.

**Status (frontier).** Implemented and measured:

- serve is same-thread and strictly younger (`isServable`/`lineForConsumer`);
- `SPACESPEC_SERVE=0` gates the serve for the A/B;
- fixed **4-cycle** serve latency, a deliberate timing-channel policy;
- the serve returns before `handleTimingReqMiss`, so no MSHR is allocated;
- `spacespec_shadowhit`: 61 serves, 61 safe, 0 unsafe, serve-off output
  byte-identical, 12 MSHR misses avoided, refreshes 0 (serve) vs 2 (off);
- the security attacks still show 463/206 serves with zero unsafe misses.

The unused `lookup()` remains as a diagnostic; the production serve path is
`lineForConsumer()`.

**Exit criteria.** The measured counter justifies the layer, all tests pass, and
the latency policy is stated in the design doc.

---

## Layer 4 — Cache hierarchy (L2/L3)

**Goal.** A speculative line does not allocate in any cache level that an
attacker can observe; at commit it is installed according to the hierarchy's
inclusion policy; on squash it leaves no trace at any level.

**Why this is a separate layer.** The current code intercepts at the L1-D
`recvTimingResp`, *after* L2/L3 have allocated the fill. It is only correct
because the SE configuration has no L2. The paper handles this with an inclusive
hierarchy (install at all levels at commit) plus a branch-ID filter that cancels
squashed transactions.

**Invariants.**

- I4.1 A speculative fill is absent from L2 and L3 before commit.
- I4.2 At commit, the line is installed in every level required by the
  inclusion policy.
- I4.3 On squash, no level retains the line.
- I4.4 No other core can observe the speculative line through a shared level.
- I4.5 The filter does not cancel a committed transaction.

**Pre-registered tests.**

| test | kind |
|---|---|
| `l2_invisible`: with `--l2cache`, probe L2 tags after a speculative miss | D/S |
| `l3_invisible`: same for L3, if modelled | D/S |
| `commit_all_levels`: commit installs in L1 and L2 | D |
| `squash_no_trace`: squash leaves L1/L2/L3 unchanged | D |
| cross-core probe (attacker on another core, shared L2/L3) | S |
| output equivalence with L2/L3 | E |
| filter: committed vs squashed transaction accounting | C |

**Exit criteria.** The tests must fail on the current code with `--l2cache` —
that is the red run, and it is the evidence that Layer 4 is real.

**Coverage gap.** The paper defers multicore coherence. If L2/L3 are shared
across cores, I4.4 is not satisfiable without Layer 6; make the scope explicit.

---

## Layer 5 — Instruction cache and TLB shadows

**Goal.** The same leak-free rule applies to instruction fetch and translation:
a speculative i-cache line or TLB entry is not installed until the owning
instruction commits.

**Invariants.**

- I5.1 No speculative i-cache line is resident before commit.
- I5.2 No speculative TLB entry is resident before commit.
- I5.3 Page-walk accesses (which use the LSQ) inherit the data-cache protection.
- I5.4 The i-cache and TLB shadow structures obey the same ownership/lifetime
  invariants as Layer 0.

**Pre-registered tests.**

| test | kind |
|---|---|
| the paper's I-cache Spectre variant (dependent branches) | S |
| a TLB probe variant | S |
| i-cache shadow unit tests (same suite as Layer 0) | U |
| output equivalence | E |
| shadow sizing / occupancy under SPEC | C |

**Exit criteria.** The I-cache attack leaks without the mechanism and does not
with it; the TLB variant likewise.

---

## Layer 6 — Coherence, multicore, TSO

**Goal.** A shadow-held line interacts correctly with snoops, invalidations,
writebacks, and other cores; a promoted line is announced in a way the coherence
protocol and snoop filter accept.

**Why it is late.** The paper explicitly leaves multicore/coherence to future
work. Doing it before Layers 4–5 would be building on an unverified hierarchy.

**Invariants.**

- I6.1 A snoop/invalidation for a held line invalidates or updates the shadow
  entry; promotion never installs stale data.
- I6.2 A promoted line's coherence state matches the hierarchy (no fabricated
  Exclusive without an acquisition).
- I6.3 A clean promoted line is evicted with the announcement the protocol
  expects (holder-bit consistency).
- I6.4 No lost invalidation and no stale promotion under TSO.
- I6.5 SMT watermarks are per-thread.

**Pre-registered tests.**

| test | kind |
|---|---|
| two-core litmus tests with a shared line | D |
| snoop while the line is in the shadow | D |
| invalidation between capture and commit | D |
| holder-bit / snoop-filter consistency | C |
| output equivalence, multicore programs | E |
| security: cross-core attacker | S |

**Exit criteria.** All of the above; plus a documented statement of which memory
model is assumed.

---

## Layer 7 — Transient speculation attacks (TSA) against the shadow

**Goal.** The shadow itself must not become a covert channel through contention,
replacement, or timing.

**Invariants.**

- I7.1 Shadow occupancy/timing does not depend on secret data in an
  attacker-observable way.
- I7.2 Full-buffer behavior does not reveal whether a capture succeeded.
- I7.3 Per-branch or per-context partitioning prevents one speculative stream
  from observing another's entries.
- I7.4 Sizing is justified by measured occupancy, not by a copied figure.

**Pre-registered tests.**

| test | kind |
|---|---|
| `tsa_replace`: Trojan replaces a shadow entry, spy observes | S |
| `tsa_full`: full buffer; measure whether the spy sees the stall | S |
| occupancy distribution over SPEC | C |
| timing difference between hit/miss/full states | P |

**Exit criteria.** The TSA microbenchmarks show no statistically distinguishable
channel; the sizing is justified by measured occupancy.

---

## Layer 8 — Performance and representativeness

**Goal.** A defensible cost number over a representative window, with the
measurement methodology stated.

**Pre-registered tests.**

- P8.1 Same instruction window for every configuration; reject mismatched
  windows automatically.
- P8.2 Warmup: a measured window after a warmup segment, not from instruction 0.
- P8.3 At least one memory-bound and one compute-bound workload.
- P8.4 Report IPC and cycles, with the window length and warmup stated.
- P8.5 Every stats file must correspond to its run: `simTicks` equals the tick
  in the `Exiting @` line (this catches mid-run snapshots).
- P8.6 A regression threshold: no layer may increase the cost on the
  compute-bound control by more than X%.

**Evidence.** A results table with provenance: gem5 commit, window, warmup,
config, and per-benchmark deltas.

**Exit criteria.** The table is reproducible from the repository, and the
methodology section answers "is this steady state?" with a measured, not
assumed, answer.

---

## 2. Cross-cutting infrastructure

These are built once and used by every layer.

1. **Stats conservation.** For every run:
   `captures == promotions + squashes + drops + remaining_occupancy + declines`.
   Any nonzero residual is a bug in the accounting or an unhandled path.
2. **Run validity check.** A stats file is usable only if `simTicks` equals the
   tick in the `Exiting @` line. A `SIGUSR1` snapshot is not a result.
3. **Determinism check.** Two runs of the same binary/config produce identical
   `stats.txt` (or the documented divergences). This is the baseline for
   differential testing.
4. **Red/green harness.** Each layer's tests live under
   `tests/test-progs/spacespec_test/` (equivalence) and
   `tests/test-progs/lvp_attack/` (security), with a per-layer runner that
   reports red/green.
5. **Mutation runner.** A small script that flips one invariant at a time and
   asserts that the layer's tests fail.

---

## 3. Current mapping and immediate next steps

- Layers 0 and 1 are implemented and have evidence. **Re-run the mutation checks**
  before treating them as frozen.
- Layer 2 is mostly done: writes/atomics/LL-SC, split loads, and per-thread
  release are in.  Secure memory is forbidden by a fatal (the shadow key has no
  secure bit), the per-thread contract is unit-tested but a full SMT run is
  still a gap, and snoop/invalidate is Layer 6.
- Layer 3 is implemented and measured: same-thread younger serve, safe/unsafe
  classification, the `SPACESPEC_SERVE=0` A/B, a fixed 4-cycle policy, no MSHR
  on the serve path, and the `spacespec_shadowhit` directed harness.
- Layer 4 is now the next layer that requires a deliberate decision: either keep
  the no-L2 constraint and document it, or implement the paper's
  non-allocating/filter mechanism. Do not add an L2 and call the result SafeSpec
  without one of those.
- Layer 5 is where the paper's scope is broader than the current code; it is a
  natural follow-on after Layer 4.

**Do not skip the red runs.** The value of this plan is not the layer list; it
is that every layer has a test that was shown to fail for the right reason before
the code existed.
