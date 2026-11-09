# SafeSpec — what was built after the LVP work

Scope: the work that followed the initial load-value-predictor attack. That
attack is the *motivation*; this document covers the defence designed in
response to it, and the design reasoning that is not visible in the code.

Implementation-specific detail is deliberately kept out. Names and structure will
change between revisions; the properties below will not.

---

## 1. The idea

A speculative cache fill must never become observable. It is diverted into a
small **shadow cache** and reaches the real cache only once the owning
instruction **commits**. If the instruction is squashed first, the line is
discarded, and there is no point in time at which the secret-dependent address
was resident in an attacker-observable structure.

The load itself is not delayed, blocked, or retired early. Speculation runs at
full width. What changes is only *where the fetched line lives*, and that is
the whole of the security argument:

| | unprotected | SafeSpec |
|---|---|---|
| speculative load | fills L1-D | fills shadow |
| instruction commits | line already resident | line installed at commit |
| instruction squashed | line already resident, **leaked** | line discarded, never existed |

A cache-line timing attack measures whether a line was resident. SafeSpec's
claim is not that measurement becomes impossible — it is that the victim's
speculative access leaves no residue to measure. Flush+Reload and Prime+Probe
both rely on that residue.

This is one level of SafeSpec (Khasawneh et al., DAC'19), which isolates
speculative state at several levels: register file, TLB, instruction cache, data
cache. Only L1-D is in scope here. That narrowing bounds the claim to L1-D and
is stated everywhere the result appears.

## 2. The problem that isn't about caches at all

The interesting difficulty is not the interception. It is **release**: deciding,
for each line, *which instruction's retirement makes it legitimate*.

Several loads can touch the same line. Exactly one of them "owns" the entry,
and that owner is the one whose commit installs the line. Get this wrong and
the mechanism fails in a way that is silent, unbounded, and looks like a hang
rather than a wrong answer.

Three rules, each of which exists because its absence caused a failure:

**Ownership is the oldest capturing load** — not the newest, not "whichever
load happens to be at the commit head". The first implementation keyed each
entry by the newest load and released it by looking up the *committing* load's
sequence number. Those two keys were never the same, so the release never
matched, so the line never reached the cache. Every access re-fetched, and
re-fetching kept re-capturing, without bound. A **refresh must not re-key**:
a younger load re-fetching a held line inherits the original owner, otherwise a
squash of the newer load strands the entry.

**Lifetime is driven from outside.** The buffer holds no reference to the
pipeline. Retirement supplies a monotonic watermark, and the buffer sweeps
entries below it. Two consequences: the buffer cannot disagree with the pipeline
about what has retired, and — more importantly — *no path can retain an entry
forever*. Every release path is best-effort; the sweep is the backstop that
makes a miss harmless rather than permanent.

**Release directions are opposite and not interchangeable.** Retirement removes
the *oldest* entries below the watermark. Squashing removes the *newest* entries
at or above the squash point. Passing a squash bound to the retirement sweep
discards the lines of loads that were about to commit. Nothing crashes; the
cache simply stops working. This is why the two operations cannot be shared.

### Why the sweep is not redundant

A load can miss, then be squashed before its fill returns. The fill arrives
anyway — a cache's miss-status-holding registers service squashed targets, since
they have no notion of squashed instructions — so the line is captured against a
sequence number that will never commit, invisible to the squash handler that had
already run. Such an entry is stranded: no release will ever match its dead
owner, and later accesses refresh it while keeping that owner. The line then
never reaches the cache and every access re-fetches, without bound.

The sweep reclaims it, because the watermark is monotonic and never reuses
sequence numbers, so it is guaranteed to pass the dead owner eventually.
Recovery is **deferred rather than immediate**: until the watermark arrives, the
line is not installed and will re-miss. That is the difference between bounded
and unbounded cost.

### Arrival order is not sequence order

Entries are held sorted by owner sequence number. Loads do not fill in program
order — an older load can miss and fill after a younger one already has — so
insertion must place entries rather than append. Keeping the order makes both
release paths a single walk from one end instead of a search.

## 3. Full-buffer policy

SafeSpec forbids evicting one speculative line to admit another: that turns the
shadow into a contention side channel, because whether a line is resident starts
depending on which speculative lines are in flight (SafeSpec §5, TSA).

So overflow is **refused, never evicted**. The structure reports the refusal and
stores nothing; the caller decides what to do about it. Crucially the two
outcomes are distinguishable — accepted versus overflow — because the caller
must not assume a refused line is there to be released.

Refusing rather than evicting converts an unbounded security problem into a
bounded availability one, which is the right trade for this technique. But it
is only the right trade if overflow is rare, and **the sizing has not been
established against a real speculative footprint**. The buffer is configured at
64 entries against a 64 KiB L1-D (6.25%), with observed peak occupancy of 5–10.
Occupancy that low means the sizing is untested, not that it is generous: no
measured workload has come close to the limit.

## 4. Layering, and why

The release logic was built and tested **before** anything was connected to a
simulator. Ownership and lifetime are the two things that actually went wrong
the first time, and neither has anything to do with gem5 — they are queue and
watermark problems. Testing them in isolation makes the failure modes
observable as wrong answers rather than as hangs.

29 unit tests on the structure alone, mutation-verified. Then the same structure
attached to a real cache. Then the attacks.

## 5. What is established

**Security.** Two independent attacks, each recovering a secret by timing cache
residency:

| attack | speculation source | unprotected | SafeSpec |
|---|---|---|---|
| Spectre V1 | branch predictor | LEAK, secret recovered | NO-LEAK, 0 recoveries |
| FLOP-style | load value predictor | LEAK, secret recovered | NO-LEAK, 0 recoveries |

The second attack exists because the defence must not be specific to one
predictor. Controls rule out the obvious alternative explanations: with the
value predictor disabled the first attack still leaks unprotected and is still
blocked protected (so the defence is not an artefact of value prediction); with
the diversion disabled the leak returns (so the protection comes from the
mechanism, not from slower simulation).

**Correctness.** Five workloads produce byte-identical output with and without
the mechanism, including cases that exercise cache pressure and deferred
completion. Diverting a fill and reinstating it later is invisible to
architectural state.

**Cost.** Measured on a 3-workload set, 3 configurations each:

- **−0.02%** where the data cache is barely touched — effectively free.
- **+2.48%** and **+7.52%** where withholding forces re-fetching.

The driver is re-fetch pressure, and it is not uniform in OoO width. At a
narrower reorder window, where memory-level parallelism cannot hide the extra
fills, the value predictor's own contribution moves from a small net loss to
**−2.04%**. That is a useful negative result: the value predictor pays for
itself mainly by sustaining throughput across a latency the mechanism now
extends, so its value is workload- and width-dependent rather than intrinsic.

## 6. Open

- **Full-system coherence.** The measurement configuration has no private L2, so
  a diverted fill has nothing below it to populate. With a lower level present,
  the fill still reaches it, and the threat model only holds if nothing below
  the guarded cache is attacker-observable.
- **Snoop filter.** One *is* active on the crossbar. The mechanism coexists
  because a promoted line is dropped without emitting any packet, so the filter
  is never asked to process an announcement for a line it does not know about.
  Nothing breaks — but the filter does not know this cache holds the line, so a
  peer's dirty write would not invalidate it here. Real, and unaddressed.
- **Write-target fills.** A fill carrying a write target cannot be diverted
  without losing the write data, so those lines proceed normally. The argument
  that this is acceptable — the line is being written by an in-flight store, so
  its footprint is not secret-dependent — is plausible but unproven.
- **Sizing.** See §3. Occupancy has never approached capacity, so the
  full-buffer path is exercised only by construction, never by measurement.
- **Shadow-hit serve (implemented).** A younger same-thread consumer may read a
  held line; the serve is gated by `SPACESPEC_SERVE`, uses a fixed 4-cycle L1
  policy (a deliberate timing-channel choice), and returns before MSHR
  allocation. The serve-off A/B keeps output identical and shows refreshes 0
  vs 2 and 12 MSHR misses avoided on `spacespec_shadowhit` (61 serves, 0
  unsafe).
- **Secure memory.** Forbidden: the shadow key has no secure bit, so a secure
  speculative fill is a fatal configuration error rather than a silently
  unprotected line. The threat model is non-secure memory only.
- **SMT.** Per-thread release is unit-tested with interleaved global sequence
  numbers; a full two-thread simulation is not yet run.

## References

- SafeSpec — Khasawneh, Nikolic, Schlӓfferl, Kastner, *DAC '19*. Source of the
  shadow-structure design, the commit policies, and the TSA sizing argument
  (§5), which is what forbids eviction on overflow.