# EgDiff base-load prevalence: how far is a sensitive load from the caller's next load?

## Question

EgDiff predicts a load's value from the value of a load `d` positions earlier in the
dynamic **load** stream, with `1 <= d <= 32`. For a victim function that loads a
sensitive value, the attack precondition is that *some* sensitive load sits at
`d <= 32` from the attacker's first load after the call, and that this position is
stable across benign inputs. This study measures that precondition on host binaries;
it does not run gem5 and does not time anything.

## Metric

Per call, with T1 = an execution of a load PC that reads the tagged key/secret page
and T2 = the marker load executed immediately after the call returns:

```
n = number of dynamic load instructions strictly between the last T1 instance
    before T2 and T2        (stores, ALU ops and branches do not advance d)
d = n + 1                   (1-based, matching EgDiff's distance)
```
Rules actually applied:

* **Minimum over all sensitive PCs, not one preselected PC.** Real functions read
  key material at several PCs (word-at-a-time verify, key expansion, copy loops).
  For each sensitive PC we take its *last* execution before T2, compute that PC's
  `d`, and report the minimum over PCs. The winning PC is reported, with its
  disassembly, so the reader can check it.
* **Last execution, not first.** Inside a loop, earlier instances are further from
  T2 and can never bind, so only the last instance of each PC is used.
* **Base-PC identity is checked, not assumed.** The PC of the `d`-th load back from
  T2 is read out of an independent ring buffer and compared with the sensitive PC
  that won; `is_sensitive_pc` records the result, and the per-PC tables below list
  every sensitive PC separately.
* **`d_key` refinement.** Where a sensitive PC is also used for non-secret data, the
  last execution of that PC may carry non-secret bytes; `d_key` uses the per-call
  key-access count measured by the fault probe to pick the last *key* read.
  `d_key == d` unless a target is marked as sharing (see the validation table).

Precondition: `d <= 32` (equivalently `n <= 31`).

## How to reproduce

```sh
cd experiment/egdiff_eval/prevalence
./tools/run_prevalence.sh          # builds, self-checks, measures, writes results/
```
Pipeline: `tools/build_targets.sh` builds static non-PIE `riscv64-linux-gnu-gcc`
targets so runtime PCs equal `objdump` addresses; `tools/load_pcs.py` classifies
loads from `objdump -d` (RVC `c.ld/c.lw/c.lwsp/c.ldsp/...` plus
`lb/lh/lw/ld/lbu/lhu/lwu/flw/fld`) and resolves marker/load PCs; `tools/trace_loads.py`
streams `qemu-riscv64 -singlestep -d exec` (never buffering it) and computes `d` per
call. Traces are piped straight into the parser; no trace file is ever written.
300 calls per input mode (`N=300`), 600-900 calls per target.

Build flags: `-static -O2 -fno-pic -no-pie -fno-ipa-icf -fno-ipa-pure-const
-fno-builtin-memcmp -fno-builtin-strcmp -fno-builtin-strlen
-fno-tree-loop-distribute-patterns`. The last four stop GCC from replacing the
control loops with library calls or deleting them; `-fno-ipa-icf` keeps the two
marker functions distinct.

## Parser validation

QEMU prints `Trace 0: <host TB addr> [<cs_base>/<PC>/<flags>/<cflags>] <symbol>`.
The guest PC is **tuple field 1** (0-based). Evidence:

```
first traced PC : 0x00000000000105a4
objdump _start  : 0x00000000000105a4
match           : yes
```
Known-load-count self-check (`build/selfcheck.c`): a call performing exactly 8 loop
loads, returning straight into the marker.

```
calls=10  d=['1']  n_loads_in_call=['8']  n_sensitive_loads=['8']
base_pc=['106a0'] == d_pc=['106a0']  is_sensitive_pc=['1']  loads_since_ret=['0']
```
For the crypto targets, the two instructions between the callee `ret` and T2 are
the store of the call's result and the `jal` into the marker; the four control
targets have three (`insns_since_ret = 3`). In every target `loads_since_ret ==
0` (see each target's table). `tools/load_pcs.py` also asserts that each
marker's load is the first load inside its symbol, so nothing can sit between
the return and T2.

## Sensitive-PC identification and dynamic validation

Static identification: intraprocedural argument provenance (`tools/load_pcs.py`)
taints `a0..a7` at function entry, follows `mv/addi/add`-style copies, and flags
loads whose base register derives from the nominated key argument. Provenance is a
conservative over-approximation, used only as corroboration.

Dynamic validation (authoritative): the tagged key page is made `PROT_NONE` for a
whole call and each faulting load is decoded and emulated out of a shadow copy in
the `SIGSEGV` handler (`targets/harness.h`). The page therefore stays protected, so
**every** key-page load in the call faults, not just the first. This yields the
sensitive-PC set and the exact per-call key-access count. Counting runs use normally
mapped pages; `bad=0` in every probe run means no unsupported load form or store hit
the page.

| target | 0 loads before T2 | static PCs | dynamic PCs | static⊆dynamic | dynamic⊆static | key accesses/call (probe) |
|---|---|---|---|---|---|---|
| t_verify32 | True | 0 | 8 | True | False | 32..32 |
| t_chacha20 | True | 4 | 4 | True | True | 32..32 |
| t_blake2b | True | 0 | 1 | True | False | 32..32 |
| t_aead | True | 0 | 4 | True | False | 32..32 |
| t_memcmp | True | 1 | 1 | True | True | 69..256 |
| t_strcmp | True | 1 | 1 | True | True | 4..65 |
| t_hashlookup | True | 2 | 2 | True | True | 1..77 |
| t_listwalk | True | 2 | 2 | True | True | 32..72 |

Static provenance returns no PC for `t_verify32` (the key is read inside
`load64_le()`, which takes the pointer as its own `a0`) and for `t_blake2b`/`t_aead`
(the key is copied by inlined code in a callee whose arguments do not include the
key). For those targets the dynamic set is the only sound source, which is exactly
why the fault probe exists. Where the two differ the measurement uses the dynamic set.
The reverse comparison is also informative: if a target's per-call sensitive-PC
executions exceed its per-call key accesses, some executions of those PCs carried
non-secret data, and `d_key` is needed. The probe and the counted run use different
input sequences, so for the four controls (whose counts are input-dependent by
design) the two ranges are not meant to match; the comparison is only meaningful for
the constant-count crypto targets.

| target | sensitive-PC executions/call (counted run) | key accesses/call (probe) | PCs shared with non-secret data? |
|---|---|---|---|
| t_verify32 | 64..64 | 32..32 | yes: shared `load64_le`; key read last, so `d_key == d` |
| t_chacha20 | 32..32 | 32..32 | no |
| t_blake2b | 32..32 | 32..32 | no |
| t_aead | 32..32 | 32..32 | no |
| t_memcmp | 1..256 | 69..256 | no (the count varies with the input path, not with buffer identity) |
| t_strcmp | 1..97 | 4..65 | no (the count varies with the input path, not with buffer identity) |
| t_hashlookup | 1..68 | 1..77 | no (the count varies with the input path, not with buffer identity) |
| t_listwalk | 32..78 | 32..72 | no (the count varies with the input path, not with buffer identity) |

## Results

One row per call in `results/prevalence.csv`

(`target,call,mode,d,base_pc,is_sensitive_pc,d_pc,d_key,base_pc_key,
n_sensitive_loads,n_loads_in_call,insns_since_ret,loads_since_ret`).

| target | calls | d mode | at mode | d min..max | distinct d | d_key mode | base PC (winner) | base==sens | 0 loads since ret | d<=32 |
|---|---|---|---|---|---|---|---|---|---|---|
| t_verify32 | 600 | 7 | 100.0% | 7..7 | 1 | 7 | `10da2` | 100.0% | 100.0% | 100.0% |
| t_chacha20 | 900 | 519 | 67.0% | 85..579 | 171 | 519 | `16154` | 100.0% | 100.0% | 0.0% |
| t_blake2b | 900 | 552 | 67.2% | 312..915 | 124 | 552 | `1681e` | 100.0% | 100.0% | 0.0% |
| t_aead | 900 | 1339 | 66.8% | 560..1462 | 174 | 1339 | `16072` | 100.0% | 100.0% | 0.0% |
| t_memcmp | 900 | 1 | 100.0% | 1..1 | 1 | 1 | `10ec4` | 100.0% | 100.0% | 100.0% |
| t_strcmp | 900 | 1 | 100.0% | 1..1 | 1 | 1 | `10e96` | 100.0% | 100.0% | 100.0% |
| t_hashlookup | 600 | 2 | 62.5% | 1..2 | 2 | 2 | `1100e` | 100.0% | 100.0% | 100.0% |
| t_listwalk | 600 | 2 | 62.5% | 1..2 | 2 | 2 | `10f24` | 100.0% | 100.0% | 100.0% |

### Per-target detail

#### `t_verify32`

* calls: **600** over modes key, msg
* `d`: mode **7**, at mode **100.0%**, min **7**, max **7**, 1 distinct value(s)
* `d_key`: mode **7**, at mode **100.0%**, max **7**
* base-PC identity: `base_pc == d_pc` in **100.0%** of calls, `is_sensitive_pc=1` in **100.0%**
* modal winning base PC `0x10da2` = `lbu a0,7(a0)`
* zero loads between the callee `ret` and T2: **100.0%** of calls
* sensitive-load count per call: **64..64**
* dynamic sensitive PCs: `10d78,10d7c,10d80,10d84,10d88,10d8e,10d98,10da2`; key accesses/call: 32..32
* **classification**: deterministic
* count-variance class: deterministic (constant-time compare)
* why: Constant-time: exactly 32 key bytes are read (4 calls of the shared `load64_le` helper, which also serves the attacker operand, hence 64 sensitive-PC executions per call), then a fixed 6-load epilogue. Nothing in the input can move d. The secret is passed second so the helper's *last* calls read the key.
* `d <= 32` holds for **100.0%** of calls

| mode | calls | d mode | at mode | d min..max | sensitive loads/call | d_key mode |
|---|---|---|---|---|---|---|
| key | 300 | 7 | 100.0% | 7..7 | 64..64 | 7 |
| msg | 300 | 7 | 100.0% | 7..7 | 64..64 | 7 |

| sensitive PC | disassembly | calls seen | d min..max | calls where it won |
|---|---|---|---|---|
| `0x10d78` | `lbu a6,1(a0)` | 600 | 14..14 | 0 |
| `0x10d7c` | `lbu a5,0(a0)` | 600 | 13..13 | 0 |
| `0x10d80` | `lbu a1,2(a0)` | 600 | 12..12 | 0 |
| `0x10d84` | `lbu a2,3(a0)` | 600 | 11..11 | 0 |
| `0x10d88` | `lbu a3,4(a0)` | 600 | 10..10 | 0 |
| `0x10d8e` | `lbu a4,5(a0)` | 600 | 9..9 | 0 |
| `0x10d98` | `lbu a5,6(a0)` | 600 | 8..8 | 0 |
| `0x10da2` | `lbu a0,7(a0)` | 600 | 7..7 | 600 |

#### `t_chacha20`

* calls: **900** over modes key, len, msg
* `d`: mode **519**, at mode **67.0%**, min **85**, max **579**, 171 distinct value(s)
* `d_key`: mode **519**, at mode **67.0%**, max **579**
* base-PC identity: `base_pc == d_pc` in **100.0%** of calls, `is_sensitive_pc=1` in **100.0%**
* modal winning base PC `0x16154` = `lbu a2,3(a3)`
* zero loads between the callee `ret` and T2: **100.0%** of calls
* sensitive-load count per call: **32..32**
* dynamic sensitive PCs: `16148,1614c,16150,16154`; key accesses/call: 32..32
* **classification**: attacker-selectable variance (length only)
* count-variance class: deterministic for fixed length
* why: The key is expanded into a stack state once, at entry; every extra plaintext block adds loads *after* the last key read, so d tracks text_size and nothing else. Key and message content are irrelevant.
* `d <= 32` holds for **0.0%** of calls

| mode | calls | d mode | at mode | d min..max | sensitive loads/call | d_key mode |
|---|---|---|---|---|---|---|
| key | 300 | 519 | 100.0% | 519..519 | 32..32 | 519 |
| len | 300 | 257 | 1.7% | 85..579 | 32..32 | 257 |
| msg | 300 | 519 | 100.0% | 519..519 | 32..32 | 519 |

| sensitive PC | disassembly | calls seen | d min..max | calls where it won |
|---|---|---|---|---|
| `0x16148` | `lbu a0,1(a3)` | 900 | 88..582 | 0 |
| `0x1614c` | `lbu a7,0(a3)` | 900 | 87..581 | 0 |
| `0x16150` | `lbu a1,2(a3)` | 900 | 86..580 | 0 |
| `0x16154` | `lbu a2,3(a3)` | 900 | 85..579 | 900 |

#### `t_blake2b`

* calls: **900** over modes key, len, msg
* `d`: mode **552**, at mode **67.2%**, min **312**, max **915**, 124 distinct value(s)
* `d_key`: mode **552**, at mode **67.2%**, max **915**
* base-PC identity: `base_pc == d_pc` in **100.0%** of calls, `is_sensitive_pc=1` in **100.0%**
* modal winning base PC `0x1681e` = `lbu a4,0(a2)`
* zero loads between the callee `ret` and T2: **100.0%** of calls
* sensitive-load count per call: **32..32**
* dynamic sensitive PCs: `1681e`; key accesses/call: 32..32
* **classification**: attacker-selectable variance (length only)
* count-variance class: deterministic for fixed length
* why: The 32-byte key is copied byte-wise into the hash context at init through a dedicated loop (a single `lbu` PC), before any message processing; the message is absorbed later through a different (64-bit) path, so the key PCs are key-exclusive. d therefore grows with message_size and nothing else.
* `d <= 32` holds for **0.0%** of calls

| mode | calls | d mode | at mode | d min..max | sensitive loads/call | d_key mode |
|---|---|---|---|---|---|---|
| key | 300 | 552 | 100.0% | 552..552 | 32..32 | 552 |
| len | 300 | 566 | 2.0% | 312..915 | 32..32 | 566 |
| msg | 300 | 552 | 100.0% | 552..552 | 32..32 | 552 |

| sensitive PC | disassembly | calls seen | d min..max | calls where it won |
|---|---|---|---|---|
| `0x1681e` | `lbu a4,0(a2)` | 900 | 312..915 | 900 |

#### `t_aead`

* calls: **900** over modes key, len, msg
* `d`: mode **1339**, at mode **66.8%**, min **560**, max **1462**, 174 distinct value(s)
* `d_key`: mode **1339**, at mode **66.8%**, max **1462**
* base-PC identity: `base_pc == d_pc` in **100.0%** of calls, `is_sensitive_pc=1` in **100.0%**
* modal winning base PC `0x16072` = `lbu a5,3(a1)`
* zero loads between the callee `ret` and T2: **100.0%** of calls
* sensitive-load count per call: **32..32**
* dynamic sensitive PCs: `16066,1606a,1606e,16072`; key accesses/call: 32..32
* **classification**: attacker-selectable variance (length only)
* count-variance class: deterministic for fixed length
* why: `crypto_chacha20_h()` is the only reader of the raw key; the whole AEAD pipeline follows it, so d tracks text_size.
* `d <= 32` holds for **0.0%** of calls

| mode | calls | d mode | at mode | d min..max | sensitive loads/call | d_key mode |
|---|---|---|---|---|---|---|
| key | 300 | 1339 | 100.0% | 1339..1339 | 32..32 | 1339 |
| len | 300 | 784 | 2.0% | 560..1462 | 32..32 | 784 |
| msg | 300 | 1339 | 100.0% | 1339..1339 | 32..32 | 1339 |

| sensitive PC | disassembly | calls seen | d min..max | calls where it won |
|---|---|---|---|---|
| `0x16066` | `lbu a3,1(a1)` | 900 | 563..1465 | 0 |
| `0x1606a` | `lbu a6,0(a1)` | 900 | 562..1464 | 0 |
| `0x1606e` | `lbu a4,2(a1)` | 900 | 561..1463 | 0 |
| `0x16072` | `lbu a5,3(a1)` | 900 | 560..1462 | 900 |

#### `t_memcmp`

* calls: **900** over modes key, len, msg
* `d`: mode **1**, at mode **100.0%**, min **1**, max **1**, 1 distinct value(s)
* `d_key`: mode **1**, at mode **100.0%**, max **1**
* base-PC identity: `base_pc == d_pc` in **100.0%** of calls, `is_sensitive_pc=1` in **100.0%**
* modal winning base PC `0x10ec4` = `lbu a4,0(a1)`
* zero loads between the callee `ret` and T2: **100.0%** of calls
* sensitive-load count per call: **1..256**
* dynamic sensitive PCs: `10ec4`; key accesses/call: 69..256
* **classification**: deterministic d; count is attacker-selectable
* count-variance class: attacker-selectable (early-exit position)
* why: The early-exit loop always ends with a load of the secret byte immediately before the return, so d=1 regardless of where the mismatch is. Only the *number* of secret-byte loads varies.
* `d <= 32` holds for **100.0%** of calls

| mode | calls | d mode | at mode | d min..max | sensitive loads/call | d_key mode |
|---|---|---|---|---|---|---|
| key | 300 | 1 | 100.0% | 1..1 | 1..2 | 1 |
| len | 300 | 1 | 100.0% | 1..1 | 1..255 | 1 |
| msg | 300 | 1 | 100.0% | 1..1 | 1..256 | 1 |

| sensitive PC | disassembly | calls seen | d min..max | calls where it won |
|---|---|---|---|---|
| `0x10ec4` | `lbu a4,0(a1)` | 900 | 1..1 | 900 |

#### `t_strcmp`

* calls: **900** over modes key, len, msg
* `d`: mode **1**, at mode **100.0%**, min **1**, max **1**, 1 distinct value(s)
* `d_key`: mode **1**, at mode **100.0%**, max **1**
* base-PC identity: `base_pc == d_pc` in **100.0%** of calls, `is_sensitive_pc=1` in **100.0%**
* modal winning base PC `0x10e96` = `lbu a4,0(a1)`
* zero loads between the callee `ret` and T2: **100.0%** of calls
* sensitive-load count per call: **1..97**
* dynamic sensitive PCs: `10e96`; key accesses/call: 4..65
* **classification**: deterministic d; count is attacker-selectable
* count-variance class: attacker-selectable (early-exit position)
* why: Same shape as memcmp: the last secret byte load is the one that terminates the loop, so d=1 while the count varies.
* `d <= 32` holds for **100.0%** of calls

| mode | calls | d mode | at mode | d min..max | sensitive loads/call | d_key mode |
|---|---|---|---|---|---|---|
| key | 300 | 1 | 100.0% | 1..1 | 1..2 | 1 |
| len | 300 | 1 | 100.0% | 1..1 | 2..97 | 1 |
| msg | 300 | 1 | 100.0% | 1..1 | 1..65 | 1 |

| sensitive PC | disassembly | calls seen | d min..max | calls where it won |
|---|---|---|---|---|
| `0x10e96` | `lbu a4,0(a1)` | 900 | 1..1 | 900 |

#### `t_hashlookup`

* calls: **600** over modes key, msg
* `d`: mode **2**, at mode **62.5%**, min **1**, max **2**, 2 distinct value(s)
* `d_key`: mode **2**, at mode **62.5%**, max **2**
* base-PC identity: `base_pc == d_pc` in **100.0%** of calls, `is_sensitive_pc=1` in **100.0%**
* modal winning base PC `0x1100e` = `lbu a6,0(a5)`
* zero loads between the callee `ret` and T2: **100.0%** of calls
* sensitive-load count per call: **1..68**
* dynamic sensitive PCs: `10ff8,1100e`; key accesses/call: 1..77
* **classification**: deterministic d; count is attacker-selectable + secret-dependent
* count-variance class: attacker-selectable + secret-dependent (probe distance)
* why: d is 1 or 2 depending on whether the lookup ends on the `used` byte (d=1) or inside the 32-byte compare, where the query-byte load follows the table-byte load (d=2). The number of probes is what varies.
* `d <= 32` holds for **100.0%** of calls

| mode | calls | d mode | at mode | d min..max | sensitive loads/call | d_key mode |
|---|---|---|---|---|---|---|
| key | 300 | 1 | 50.0% | 1..2 | 1..68 | 1 |
| msg | 300 | 2 | 75.0% | 1..2 | 1..61 | 2 |

| sensitive PC | disassembly | calls seen | d min..max | calls where it won |
|---|---|---|---|---|
| `0x10ff8` | `lbu a5,0(a7)` | 600 | 1..65 | 225 |
| `0x1100e` | `lbu a6,0(a5)` | 542 | 2..3 | 375 |

#### `t_listwalk`

* calls: **600** over modes key, msg
* `d`: mode **2**, at mode **62.5%**, min **1**, max **2**, 2 distinct value(s)
* `d_key`: mode **2**, at mode **62.5%**, max **2**
* base-PC identity: `base_pc == d_pc` in **100.0%** of calls, `is_sensitive_pc=1` in **100.0%**
* modal winning base PC `0x10f24` = `lbu a2,0(a5)`
* zero loads between the callee `ret` and T2: **100.0%** of calls
* sensitive-load count per call: **32..78**
* dynamic sensitive PCs: `10f24,10f34`; key accesses/call: 32..72
* **classification**: deterministic d; count is attacker-selectable + external-state dependent
* count-variance class: attacker-selectable + external-state dependent (walk length)
* why: d is 1 or 2: ending on the `next` pointer gives d=1, ending inside the 32-byte compare gives d=2 (the query-byte load follows the node-byte load). The walk length is what varies.
* `d <= 32` holds for **100.0%** of calls

| mode | calls | d mode | at mode | d min..max | sensitive loads/call | d_key mode |
|---|---|---|---|---|---|---|
| key | 300 | 1 | 50.0% | 1..2 | 32..78 | 1 |
| msg | 300 | 2 | 75.0% | 1..2 | 32..78 | 2 |

| sensitive PC | disassembly | calls seen | d min..max | calls where it won |
|---|---|---|---|---|
| `0x10f24` | `lbu a2,0(a5)` | 600 | 2..3 | 375 |
| `0x10f34` | `ld a0,0(a0)` | 583 | 1..65 | 225 |

## Overall answer

* `d <= 32` for **all** calls (precondition holds): `t_verify32`, `t_memcmp`, `t_strcmp`, `t_hashlookup`, `t_listwalk`.
* `d > 32` for **all** calls (precondition fails): `t_chacha20`, `t_blake2b`, `t_aead`.
* Input-dependent: none.

Stability: the winning base PC is **always one of the target's sensitive PCs**,
and the per-PC tables above show each sensitive PC's own distance. It is a single
PC in every call for `verify32`, `memcmp` and `strcmp`. For `hashlookup` and
`listwalk` it alternates between their two sensitive PCs (225/375 and 375/225
calls), so the summary `d` is a minimum over the two rather than a single stable
base. `d` is a single value for `verify32` (7), `memcmp` (1) and `strcmp` (1); it
is 1 or 2 for `hashlookup`/`listwalk`, depending on which of their two sensitive
loads ends the exit path; and for `chacha20`/`blake2b`/`aead` it is constant within each of the
`msg` and `key` modes (100% at mode) and a deterministic function of `text_size` in
`len` mode - message and key *contents* never move it. What varies with the input is
the *number* of sensitive loads executed - the early-exit position in
`memcmp`/`strcmp`, the probe count in the hash table, the walk length in the list -
not the distance from the last sensitive load to the caller's next load. For a
value predictor that is the useful asymmetry: the exit tail is short and stable, while
the amount of secret-dependent work before it is not.

Where the precondition fails it fails structurally: a multi-block primitive reads its
key once at entry and then performs hundreds of loads of derived state, so no
key-buffer load can be within 32 loads of the caller's next load.

## Caveats

* QEMU user-mode traces the **architectural** instruction stream. EgDiff's distance is
  in dispatch/allocation order, which can also contain wrong-path loads on mispredicted
  branches that are later squashed. The measured `d` is therefore an optimistic
  architectural exit-tail distance. This matters most for the data-dependent-branch
  controls (`t_memcmp`, `t_strcmp`, `t_hashlookup`, `t_listwalk`), where a
  misprediction at the exit test would inject extra wrong-path loads after the last
  real one.
* The metric is PC-indexed, as EgDiff is. A PC in the sensitive set may also read
  non-secret data: in `t_verify32` the shared `load64_le` helper is used for both
  operands, so 64 sensitive-PC executions occur per call against 32 key accesses. The
  secret is passed second, so the *last* helper calls read the key and `d_key == d`.
* The `PROT_NONE`/`SIGSEGV` probe marks **every load that touches the tagged page**,
  which is an over-approximation of "reads the secret". For the synthetic controls
  this matters: `t_listwalk`'s `d = 1` winner is `ld a0,0(a0)` (the next pointer) and
  `t_hashlookup`'s `d = 1` entry is the `used` byte. The actual secret load is still
  within `d <= 3`, so the conclusion is unchanged, but the summary `d` is optimistic
  for those two targets.
* `base_pc == d_pc` and `d_key == d` hold in all 6,300 rows. Because the winning PC
  is selected from the sensitive set, these are consistency checks, not independent
  evidence that the base is the secret load. The independent evidence is the
  `objdump` attribution of each sensitive PC and (for A4) the gem5 `base_val` trace.
* Only architectural, single-threaded, caller-controlled calls are measured. No
  speculation, cache, TLB or interrupt effects are modelled.
* Loads are classified from `objdump` mnemonics; atomics (`lr.w`/`lr.d`) are excluded
  (no target uses them). Compressed and plain load forms are both handled.
* The fault-PC table holds 4096 distinct PCs; per-call key-access counts use an
  uncapped counter, so the counts are exact. `bad=0` in every run.
* Sensitive data lives in page-aligned, tagged `mmap` pages (`EGDIFFv1-KEYBUF:`), not
  on the stack, so the probe protects exactly one page.
* The `msg`/`key`/`len` modes are separate runs with the same fixed PRNG seeds, so the
  same input sequence is replayed in each mode; they are not interleaved.
* `hashlookup` and `listwalk` rebuild a 2.5 KB table / 24-node list per call in `key`
  mode; those rebuilds are outside the measured window (before the begin marker).

## Upstream corpus

* Monocypher: <https://github.com/LoupVaillant/Monocypher>
* Commit: `f8bb8dbd224556d58f5b4fa176c3f18fbfe50ebe` (2026-11-22)
* Vendored unmodified at `third_party/Monocypher/` (git-ignored, never committed).
* Functions used: `crypto_verify32`, `crypto_chacha20_djb`, `crypto_blake2b_keyed`,
  `crypto_aead_lock`; signatures checked against `src/monocypher.h`.
* Toolchain: `riscv64-linux-gnu-gcc` (Debian 12.2.0-13), `qemu-riscv64` 7.2.22
  (Debian 1:7.2+dfsg-7+deb12u18+b3).

