# EgDiff base-load prevalence

**Question.** EgDiff (ISCA 2026) predicts a load's value from the value of a load
`d` positions earlier in the dynamic **load** stream, with `1 <= d <= 32`. For a
victim function that loads a sensitive value, an attacker controlling the caller
needs its own load (`L_atk`) to sit at a small `d` from that sensitive load. This
experiment measures, on host RISC-V binaries, *how far* the sensitive load really
is and *how stable* that distance is across benign inputs. It is a precondition
check only: no gem5, no timing, no simulator changes.

**Metric.** Per call, with T1 = an execution of a load PC that reads the tagged
key/secret page and T2 = a marker load executed immediately after the call
returns:

```
n = number of dynamic load instructions strictly between the last T1 before T2 and T2
d = n + 1                        (1-based, matching EgDiff's distance)
```

Only loads advance the queue; stores, ALU ops and branches do not. For every
sensitive PC we take its **last** execution before T2, and the reported `d` is the
**minimum over all sensitive PCs** — the question is whether *any* sensitive load
sits in the window. The winning PC is reported with its disassembly, and the PC of
the `d`-th load back from T2 is read out of an independent ring buffer and checked
against the sensitive set (`is_sensitive_pc`). The precondition is `d <= 32`.

A secondary `d_key` refines `d` when a sensitive PC is also used for non-secret
data: it uses the per-call key-access count measured by the fault probe to pick the
last execution that provably reads the key. `d_key == d` except where noted.

**Result (short).** `d <= 32` holds for every call of `crypto_verify32` (d=7),
`memcmp` (d=1), `strcmp` (d=1), open-addressing hash lookup (d=1–2) and linked-list
walk (d=1–2). It fails for every call of `crypto_chacha20_djb` (85–579),
`crypto_blake2b_keyed` (312–915) and `crypto_aead_lock` (560–1462), because those
primitives read the key once at entry and then do hundreds of loads of derived
state. In every target the same sensitive PC wins in every call, and `d` is a
single value for `verify32`/`memcmp`/`strcmp`, 1-or-2 for the hash lookup and list
walk, and constant within each `msg`/`key` mode for the three block primitives
(a deterministic function of the caller's length in `len` mode). What varies with
the input is the *number* of sensitive loads executed, not the exit-tail distance. See `results/prevalence.md` for the full tables,
per-PC breakdowns, variance classification and caveats.

**Reproduce.**

```sh
cd experiment/egdiff_eval/prevalence
./tools/run_prevalence.sh          # builds, self-checks, measures, writes results/
```

That runs, in order:

1. `tools/build_targets.sh` — static, non-PIE `riscv64-linux-gnu-gcc` targets
   (`-static -O2 -fno-pic -no-pie`, plus `-fno-ipa-icf -fno-ipa-pure-const
   -fno-builtin-memcmp -fno-builtin-strcmp -fno-builtin-strlen
   -fno-tree-loop-distribute-patterns`) so runtime PCs equal `objdump` addresses and
   the control loops survive optimisation.
2. A parser self-check on a program with a known number of loads per call
   (`8` loads, `d = 1`, `base_pc == d_pc`, zero loads between the callee `ret` and
   T2). The run stops if any of that is off.
3. Per target: derive load PCs and marker PCs with `tools/load_pcs.py`, run the
   `PROT_NONE` fault probe, then stream `qemu-riscv64 -singlestep -d exec` through
   `tools/trace_loads.py` for each input mode (300 calls per mode).
4. Regenerate `results/prevalence.md` from `results/prevalence.csv`.

**Inputs / variance.** Each target is run in up to three modes over 300 calls each
with fixed PRNG seeds: `msg` (attacker-selectable data varies), `key` (the secret
varies), `len` (the length parameter varies, where the API has one). Controls that
have no key/length parameter run the applicable subset.

**Sensitive-PC identification.** Statically by intraprocedural argument provenance
in `tools/load_pcs.py` (taint `a0..a7`, follow `mv`/`addi`/`add`), and dynamically —
authoritatively — by protecting the tagged key page with `PROT_NONE` and decoding +
emulating each faulting load out of a shadow copy inside the `SIGSEGV` handler
(`targets/harness.h`). Because the page stays protected for the whole call, *every*
key-page load faults, which yields both the PC set and an exact per-call key-access
count. Counting runs use normally mapped pages. `bad=0` in every probe run.

**Caveats.**

* QEMU user-mode traces the **architectural** stream. EgDiff's distance is in
  dispatch/allocation order, which can also contain wrong-path loads on mispredicted
  branches that are later squashed; the measured `d` is therefore an optimistic
  architectural exit-tail distance. This matters most for the data-dependent-branch
  controls (`memcmp`, `strcmp`, hash lookup, list walk).
* The metric is PC-indexed, as EgDiff is: one `crypto_verify32` sensitive PC is the
  shared `load64_le` helper, which serves both operands (64 sensitive-PC executions
  per call against 32 key accesses). The secret is deliberately passed as the second
  operand so the helper's last calls read the key.
* No speculation, cache, TLB or interrupt effects are modelled; only single-threaded
  caller-controlled calls are measured.
* Atomics (`lr.w`/`lr.d`) are not classified as loads; no target uses them.

**Upstream corpus.** Monocypher, <https://github.com/LoupVaillant/Monocypher>,
vendored unmodified at `third_party/Monocypher/` (git-ignored, never committed) at
commit `f8bb8dbd224556d58f5b4fa176c3f18fbfe50ebe` (2026-11-22). Functions used:
`crypto_verify32`, `crypto_chacha20_djb`, `crypto_blake2b_keyed`, `crypto_aead_lock`
(signatures checked against `src/monocypher.h`). Toolchain: `riscv64-linux-gnu-gcc`
12.2.0 (Debian 12.2.0-13), `qemu-riscv64` 7.2.22 (Debian 1:7.2+dfsg-7+deb12u18+b3),
`riscv64-linux-gnu-objdump` 2.40.

**Files.**

```
targets/harness.h            markers, tagged page-aligned buffers, fault probe
targets/t_*.c                one binary per target; loops over inputs internally
tools/build_targets.sh       build + objdump dump
tools/load_pcs.py            objdump load classification, provenance, marker PCs
tools/trace_loads.py         QEMU trace streaming, per-call d computation
tools/run_prevalence.sh      full pipeline + self-checks + report generation
results/prevalence.csv       one row per call (6300 rows)
results/prevalence.md        tables, distributions, classification, verdict, caveats
```
