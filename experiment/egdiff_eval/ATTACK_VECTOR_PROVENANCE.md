# Attack-vector provenance and pruning

This note records the original EgDiff attack-vector brainstorm and why the
unused vectors were dropped. It is historical context, not a paper claim.

## Source

- Session: `ses_dfb917269ffeY0R4ofqjhTL5CL`
- Title: "Assessing new LVP attack vectors from predictor structure"
- Initial proposal model: `gemini-4-argon`
- The initial list proposed **7 vectors**. A zero-bias critic subagent then
  accepted four and rejected three. Later work implemented only two mechanisms
  and kept one paper-faithful claim.

## Funnel

```text
7 proposed
  -> 4 accepted by the critic
  -> 2 implemented end-to-end (SpecGVQ Mode 1, NVQ Mode 2)
  -> 1 kept as the paper-faithful claim (strict SpecGVQ + crafted Mode 1 + A4)
```

## The original seven

| # | Vector | Fate | Reason |
|---|--------|------|--------|
| 1 | **SpecGVQ value teleportation** | **Kept**; became Mode 1 and A4 | Strongest result: the attacker's own load consumes an earlier victim load's value by distance alone, with no PC collision and no victim-side value-to-address gadget |
| 2 | **Verification-comparator equality oracle** | **Dropped later, not implemented** | Accepted by the critic as a narrow 8/16-bit timing oracle under cache-invisible defenses, but it requires precise LQ-verification vs branch-squash timing and targets SafeSpec/InvisiSpec/DoM. Defense evaluation was deferred/out of scope; the implemented attacks use a cache transmitter instead |
| 3 | **NVQ capture and replay via Algorithm 1** | Implemented as **Mode 2**, then excluded from the paper | Uses `NVQ` as a prediction base. The paper describes prediction from the speculative queue; the `NVQ` base is our Rule C extension, not paper-faithful. Retained as an extension/conformance experiment |
| 4 | **GVQ frame-shift** | Rejected | Requires modifying the victim's dynamic load stream; the attacker does not control victim code. The 32-load horizon and LQ verification (~cycle 6) close the window before an outer 21-cycle branch can squash |
| 5 | **Hybrid EVES -> SpecGVQ -> EgDiff** | Accepted narrowly, **not implemented** | EVES-assisted early population is an optional hybrid, not base EgDiff. The model does not implement EVES, and the hybrid was explicitly out of scope |
| 6 | **Global `lastMisp` blackout side channel** | Rejected | 1,024-instruction window is only ~128--300 cycles on the 8-wide core; the 21-cycle squash penalty dominates. Too weak for reliable cross-PC data extraction |
| 7 | **`Wake` / PRF-port contention** | Rejected | Roughly one-cycle effect in <1% of cycles; completely overshadowed by the 21-cycle squash penalty |

The critic also rejected a **single-call NVQ brute-force** variant (sometimes
called Protocol 3B):

- a one-shot victim puts the secret in `NVQ`, but leaves `ctr = 0`;
- at `ctr = 0`, EgDiff does not predict;
- testing a guess requires the attacker load to commit;
- the first wrong guess makes Algorithm 1 overwrite `diff`, destroying the
  captured value.

NVQ capture therefore requires a repeatable victim and ~42 calls (Protocol 3A /
Mode 2).

## Why vector 2 was dropped

Vector 2 survived the critic but was **not implemented**. It was dropped for
three practical reasons:

1. It is a different observation channel: LQ-verification timing and branch
   shadow, not the cache-fill channel used by Mode 1/A4.
2. It is specifically interesting against invisible-speculation defenses
   (SafeSpec/InvisiSpec/DoM); defense evaluation was deferred and declared out
   of scope.
3. It requires a narrow-secret setup (8/16-bit) and careful cycle alignment
   between LQ verification and the branch-squash point. That was not built or
   validated.

The later session explicitly notes that only Mode 1, Mode 2 and Layer 5 use a
cache probe; the equality-oracle variant remained unimplemented.

## Why A4 and SipHash are not in the original seven

- **A4 (`crypto_verify32`)** was added later as a real-library target for the
  SpecGVQ forwarding result (`k[24..31]`, `d = 7..14`).
- **SipHash** was added later as a real-library target. Strict SpecGVQ fails
  0/16; 16/16 requires `EGDIFF_NVQ_BASE=1`. It is therefore an NVQ-extension
  result, not a paper-faithful SpecGVQ result, and was excluded from the paper
  draft.

## Scope exclusions that also apply

- single core, single thread, same address space only;
- no privilege/cross-process boundary;
- full 32-byte `crypto_verify32` key not recovered in one call;
- `memcmp`/`strcmp`/hash/list are prevalence measurements, not attacks;
- block primitives (`chacha20`, `blake2b`, `aead_lock`) are outside the
  32-load window;
- defence evaluation deferred.

## Later empirical caution: prefetcher and full cache hierarchy

The paper-style configuration includes a degree-4 L1 stride prefetcher. In the
A4 cache observation:

- prefetcher **off**, full L2/L3: A4 recovers 8/8, 15--16 votes/lane;
- prefetcher **on**, full L2/L3: A4 reports 0/8 and all lanes `UNKNOWN`;
- the first failure mode is the prefetcher making the probe scan uniformly hot,
  collapsing `t_cold` to the hit latency; the hot/cold gate then rejects every
  attempt before voting.

The sentinel-layout experiment (using a measured line or a separate
page-aligned line) did not fix this. The prefetcher is the remaining obstacle
for A4, not the queue forwarding.

### A4 sentinel-layout subagent result

A background subagent tested the hypothesis that the separate 257th sentinel
line was aliasing into the 256-line probe array:

1. sentinel moved inside the 256 measured lines (line 255);
2. sentinel moved to a separate page-aligned array, timed before the scan.

Both failed with the prefetcher enabled (`29 passed, 7 failed`; all eight lanes
`UNKNOWN`, 0/8 bytes). With the prefetcher disabled, the same baseline source
recovers 8/8 with 15--16 votes per lane.

The decisive debug line under the enabled prefetcher was:

```text
DBG d=13 a=0 t_arch=13 t_cold=13 th=13 min=13 max=37 SKIP
```

The scan is uniformly hot: `t_cold` has collapsed to the hit latency, so the
hot/cold gate skips every attempt before any candidate vote. Sentinel aliasing
is therefore not the cause; the degree-4 stride prefetcher is filling the probe
lines. The subagent reverted its experimental edits.

Relevant logs:

- `/tmp/opencode/egdiff_a4_pf_inside_exp2_run.log`
- `/tmp/opencode/egdiff_a4_pf_inside_run.log`


## Reproduction paths

- Vector history: session database
  `~/.local/share/opencode/opencode.db`, session
  `ses_dfb917269ffeY0R4ofqjhTL5CL`.
- A4 experiment logs:
  - `/tmp/opencode/egdiff_a4_pf_inside_exp1_run.log`
  - `/tmp/opencode/egdiff_a4_pf_inside_exp2_run.log`
