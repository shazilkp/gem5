# SpaceSpec — working notes

Notes on implementing SafeSpec-style cache defence in gem5's O3 CPU:
what the technique requires, what gem5 makes hard, and how to go about
verifying it.

Deliberately **not** a log of any particular implementation. Structure and
naming will change between revisions; these are the things that don't.

---

## The idea in one paragraph

A speculative cache fill is never allowed to become observable. It is diverted
into a small **shadow cache**, and reaches the real cache only once the owning
instruction **commits**. If the instruction is squashed first, the line is
discarded and never existed anywhere an attacker can time. Under the wait-for-
commit policy this defeats both Spectre (transient state) and Meltdown
(stale-or-younger data).

SafeSpec (Khasawneh et al., DAC'19) isolates speculative state at several
levels — register file, TLB, instruction and data caches. What is in scope
here is the L1 data cache only. That is a deliberate narrowing, and it means
the security claim is only as strong as that narrowing.

---

## Why the fill must be intercepted in the cache

The intuitive hook — the point where a load receives its data — **cannot
work**. By then the L1 has already allocated the block and copied the data in.
A hook there can observe the fill but cannot prevent it.

Intercepting inside the cache is not just more correct, it is *cheaper*:

- **The fill packet carries the whole line.** The response delivered to the
  pipeline carries only the bytes the load asked for. Any scheme that wants to
  hold a full line must catch it before that split happens.
- **gem5 already has the mechanism.** Forcing a fill to not allocate routes it
  through existing temporary-block scratch space. Blocking the install is a
  one-line change rather than new cache machinery, and the MSHR is still
  serviced normally, so the load completes at unchanged latency with unchanged
  data.

The general lesson: **intercept at the lowest layer that still has the full
information, even if that layer is more invasive.** The pipeline is more
convenient and strictly less capable.

---

## gem5 constraints that shape any implementation

These are properties of gem5, not opinions. Each one has a concrete cost.

| Constraint | Consequence |
|---|---|
| The snoop filter learns cache residency only from **fill responses crossing the xbar** | A line installed by any other route is invisible to it, and its eventual eviction trips a "sender is not a holder" panic |
| `BaseCache::access()` sets **DirtyBit** on any writeback | A clean line cannot be promoted by pushing a writeback upward; it would be marked dirty and corrupt replacement state |
| The fill path and any scratch path share **one temp block** | Usable only under gem5's existing non-reentrancy assumption: consumed fully, invalidated, no event processing in between |
| An MSHR whose targets need a **writable copy** writes that data into scratch space | Such fills **cannot** be diverted — the write data would be discarded with the scratch block. This is a real gap in the guarantee |
| Atomics (LL/SC, AMO, CAS) require a **stable resident block** | Perturbing one makes a store-conditional fail indefinitely — a livelock, not a slowdown |
| `Commit::tick` adjusts the squash point when the squashing instruction is included | `ROB::squash()` receives an **already-exclusive** bound, so a plain `>` comparison matches the pipeline exactly |
| `ccprintf` supports **no length modifiers** | `%zu` renders as `<bad format>u`, silently |
| `--param` execs with globals bound only to **existing child** SimObjects | A `NULL`-defaulted param cannot be instantiated from the command line; a config script is required |
| `-d` is the outdir **global** flag | It must precede the config script, or it parses as a debug flag and is rejected |

### Consequences worth stating explicitly

**A promoted line is clean and unannounced.** It was fetched from memory and
never written, so memory remains authoritative — which is what makes it safe to
drop silently rather than announce. Two things follow:

- a **store** breaks both assumptions (it makes the line dirty and dirty lines
  must be announced), so a store to such a line has to re-fetch normally;
- **atomics must be excluded** entirely.

**Diverting a fill without a way to release it is a livelock.** Interception
alone means the line is never in the cache, so every later access to it misses,
re-fetches, and re-diverts — forever. Interception and promotion are a single
unit; neither is useful alone.

**Binding a shadow entry's lifetime to one instruction is fragile.** Any
delay or skip in that single owner's commit turns into unbounded re-fetching.
A watermark or an explicit expiry is more robust than a per-entry owner.

---

## Where the subtle bugs live

Not to enumerate specific defects, but to note *what class of thing* reliably
produces them, because these are where effort should go.

| Class | Why it is hard |
|---|---|
| **Entry keying / ownership** | "Which instruction owns this line?" has no obvious answer when several loads touch the same line. Each plausible choice creates a different livelock. |
| **Ordering between shadow copy and in-flight fill** | Two independent facts about the same address. Answering a load from a shadow copy without consulting pending MSHRs or the write queue can return stale data. Discarding a shadow copy does not cancel a fill already in flight, which can install older data afterwards. |
| **Request-queue state summaries** | The most expensive class, and the one least guarded by testing. A miss queue exposes *summaries* of its state — "does this need a writable copy?", "does this have any pending targets?" — and each summary is individually accurate about the list it reads. Several lists exist, however, and a request can be on one while absent from another. Believing a summary that is true-but-narrow is how a mechanism ends up dropping a write that was waiting behind a read. Query the aggregate, not the convenient field. |
| **Work that arrives after the event that would have handled it** | A fill for an already-squashed access still arrives, because miss queues have no notion of squashed instructions. Anything keyed on the *event* rather than on *state* has a window in which the world has moved on and the handler never runs again. State-driven reclamation is the only kind that closes its own window. |
| **Downgrading "eligible" to "actual"** | The pipeline decides speculation at dispatch, long before hit/miss is known. Anything upstream keyed on that decision is a guess, and guesses become wrong flags that are very hard to spot in a trace. |
| **Partial-line captures** | Capturing only the requested bytes rather than the full line, then serving a different offset from that partial copy, yields garbage that looks plausible. |
| **"It doesn't crash" as a criterion** | See below. This is the most expensive trap of all. |

---

## How to verify it

### Output equivalence, not absence of crashes

The discipline that catches bugs at their own layer:

> At every stage, run a set of binaries with the mechanism on and off and diff
  program output byte-for-byte. **Output mismatch is the bug signal.**

"It doesn't segfault" is not a test. It is a symptom detector, and a poor one:
a fault deep in a program is often thousands of instructions from its cause, so
the reported location actively misdirects. Output equivalence localises bugs to
the stage that introduced them, because a stage that preserves output cannot
have broken it.

### Microbenchmarks probe mechanism, not correctness

A small synthetic benchmark is good for answering *"does this engage at all?"* —
it produces a readable state-transition trace (tag → capture → serve → promote →
release). It is close to useless for *"is this correct?"*, because it is small,
uniform, and misses the interesting cases: it typically has no control flow,
few stores, and a working set smaller than the structure being tested. It can
never generate pressure on something sized for real workloads.

Use it as an instrument. Not as proof.

### Which tests catch which bug class

| Bug class | Test that catches it |
|---|---|
| Ordering vs. in-flight fill | A store to a line an in-flight load is holding |
| Shadow too small / lifetime | Working set much larger than the shadow, so the full path fires |
| Atomic perturbation | Anything using locks or atomics — including most libc start-up |
| Wrong data returned | Store-heavy and parse-heavy code, not a pointer chase |
| Dropped or reordered write | A store to a line whose read miss is *still outstanding*, so the store must queue behind it |
| Lifetime livelock | Repeated access to one line across commit boundaries |

Build this set **before** the mechanism, not after something breaks.

### A test can pass while testing nothing

The most useful lesson from building this, and one worth more than any row
above:

> A store-heavy correctness test whose working set **fits in the cache** does
> not test stores. Every store hits a resident block, so the entire miss path —
> where write-allocate, write buffering and miss-queue target lists live — is
> never executed. The test passes, looks thorough, and would not notice a
> mechanism that loses every store it intercepts.

Size is not the only way to get this wrong. A "flush" helper that does nothing
removes the same coverage while still reading as deliberate. Check that a test
genuinely misses before trusting it: assert on the miss counter, or size the
working set past the largest cache in the configuration.

The corollary is that **a newly added test must be shown to fail** against the
broken variant it was written for. A test that has never been observed red is
not evidence.

### Prefer a wrong answer to a crash

Silent corruption is harder to attribute than a loud failure, and much harder to
rule out later. When choosing between a mechanism that produces a wrong result
and one that trips an assertion or panics on the inconsistent state, take the
loud one: it names the invariant it caught, at the moment it was caught.

This cuts both ways. A deliberate `assert` on an invariant that is supposed to
hold is one of the cheapest bug detectors available, precisely because it fires
at the point of violation rather than at the point of consequence.

### Debugging affordances worth having from the start

- **Runtime feature gates** (env var), not `if (false && ...)` edits. They let
  a correctness problem be bisected against a path without edit-and-rebuild,
  and they make "does it still fail with X off?" a one-line question.
- **A debug flag with a `DPRINTF` on every state transition.** The transition
  trace is what localises every bug found so far.
- **Runtime assertions on invariants** (occupancy bounded, ownership
  consistent), because `assert` compiles out of release builds exactly where a
  silent invariant violation becomes a hang.

### On `if (false && ...)` as debugging

Used to disable a path mid-diagnosis. It is worse than a runtime gate three
ways: it reads as if the feature is conditional; it silently disables the
feature, so a vanishing symptom may just be changed timing rather than a real
fix; and it survives in the diff, where it gets missed and later mistaken for
a real condition. Gate at runtime instead.

---

## Design questions still open

Ordered by how much they affect the shape of the implementation.

1. **What happens when the shadow is full?**
   SafeSpec stalls the pipeline: evicting one speculative line to admit another
   turns the shadow into a contention side channel (§5, TSA). Stalling, though,
   needs a liveness argument — the release path must not depend on anything
   that can wait on the structure being full.

   Alternatives worth weighing: stall with a proven liveness argument; evict with
   an explicit, documented weakening of the TSA argument; size the shadow so it
   effectively never fills, which is what SafeSpec's 64-entry figure assumes and
   has **not** been verified against measured occupancy on a real workload.

2. **How is ownership of a shadow entry defined?**
   Several loads can touch one line. Tying lifetime to a single owner is
   fragile (§ above). A commit watermark, or expiry by age in the speculation
   window, avoids the failure mode more cleanly.

3. **Is the write-target gap acceptable?**
   Fills that carry a write target cannot be diverted without losing the write
   data, so those lines reach the cache normally. The argument that this is
   acceptable — such a line is being written by an in-flight store, so its
   footprint is not secret-dependent — is plausible but **not proven**. It needs
   a decision and an honest write-up either way.

4. **L1-D only — what does that cost the claim?**
   With a lower cache in the hierarchy, a diverted fill still populates the
   level below. The threat model is only satisfied if nothing below L1 is
   attacker-observable. On a configuration with no private L2 this holds
   naturally; elsewhere it must be arranged deliberately.

   The original gem5 SE configuration happens to build **no** L2 under
   `--caches`, so it satisfies this incidentally — which is a trap, because it
   will silently stop holding if the memory system changes.

5. **No security test exists yet.**
   The point of the exercise is a Spectre-style gadget with an aliasing
   measurement, run with and without the mechanism. Nothing of the kind has been
   built, so the security claim is currently supported only by argument.

6. **How much does it cost?**
   Overhead is the other half of the result and is not yet measured on
   anything representative.

---

## References

- SafeSpec — Khasawneh, Nikolic, Schlӓfferl, Kastner, *DAC '19*: temporally
  and spatially isolated speculative state. Source of the shadow-structure
  design, the commit policies (WFB/WFC), the TSA sizing argument, and Fig. 3's
  sizing figure.
- InvisiSpec — the same group's follow-on work, worth reading for how SafeSpec's
  per-level shadows compose.
- FLOP — the basis for the LVP already present in this tree, and the reason the
  two mechanisms compose: both funnel through the same squash path, so one hook
  covers both.