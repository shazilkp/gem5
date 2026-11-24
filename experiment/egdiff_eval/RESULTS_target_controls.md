# Real-target controls — warm-up and input variation

The independent review (R5/R6) argued that the real-target results depend on an
undisclosed warm-up call and on constant victim inputs.  We added compile-time
switches to test both.

## A4 — Monocypher `crypto_verify32`

`COLD_KEY=1` removes the two warm-up `target_verify32` calls and the
key-reading `target_verify32_selfcheck`.  `VARY_GUESS=1` refills the 32-byte
guess on every measurement attempt.

| variant | result |
|---|---|
| warm, fixed guess (baseline) | MATCH, 8/8, key word `a1108f2ebd4cdb6a` |
| cold key, fixed guess | **MATCH, 8/8** |
| warm, varying guess | **MATCH, 8/8** |
| cold key, varying guess | **MATCH, 8/8** |

So in this model the warm-up is **not necessary**; the cold-key variant still
recovers the last key word.  Varying the guess does not trigger a
victim-side value-misprediction blackout that blocks the attack.  The most
likely reason is that the shared `load64_le` PC sees alternating key/guess
values and does not saturate its table entry, so the victim's own loads do not
provide a stable prediction to mispredict.

These are still narrow tests: one key word, a zero-spill static ABI, a lane map
measured offline, and a synthetic caller.  They answer the specific “warm-up /
constant input” objection, not the broader external-validity question.

## SipHash-2-4

`COLD_KEY=1` removes the `g_sink += victim_tag() & 1` warm-up call.

| variant | result |
|---|---|
| warm (baseline) | MATCH, 16/16, k0/k1 exact |
| cold key | **MATCH, 16/16** |

The cold variant still recovers both key words.  The empty-message path and the
one-call-per-byte structure remain limitations.

## Independent audit additions

The zero-knowledge audit rebuilt A4 with a **different key word**
(`0x8877665544332211`) and recovered exactly that word, 8/8 — the leak tracks
the victim's secret rather than a hard-coded harness value. It also built a
stub in which the key loads are removed: the estimator still reports
`known=ff` and eight zero bytes, and the oracle reports `NO-MATCH`. That is the
expected semantics of `known`: it is an emission mask, not a provenance
certificate. See `AUDIT_zero_knowledge.md`.

## What this changes

- The “Mode 1 = pure in-flight SpecGVQ” label is too narrow.  The real-target
  runs use the same untagged queue through **SpecGVQ or NVQ**; the A4 trace has
  many `NVQ` bases and `base_pc=0`.  The claim is retitled **GVQ/NVQ
  forwarding**.
- The warm-up is disclosed and, in this model, not required.
- `fence.i` is still a harness drain, not a RISC-V data fence; it is outside
  the transient window and does not change the mechanism.

Reproduce:

```bash
MONO=experiment/egdiff_eval/prevalence/third_party/Monocypher/src
for cfg in "0 0 warm" "1 0 cold" "0 1 vary" "1 1 coldvary"; do
  set -- $cfg
  riscv64-linux-gnu-gcc -static -O2 -fno-pic -no-pie -fno-ipa-icf -I"$MONO" \
    -DCOLD_KEY=$1 -DVARY_GUESS=$2 -o /tmp/poc_a4_$3 \
    tests/test-progs/egdiff_attack/poc_verify32.c \
    tests/test-progs/egdiff_attack/target_verify32.c "$MONO/monocypher.c"
done

riscv64-linux-gnu-gcc -static -O2 -fno-pic -no-pie -DCOLD_KEY=1 \
  -o /tmp/siphash_cold tests/test-progs/egdiff_attack/siphash_attack.c \
  tests/test-progs/egdiff_attack/egdiff_secret.c
```
