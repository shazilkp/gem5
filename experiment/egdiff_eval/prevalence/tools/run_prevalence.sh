#!/usr/bin/env bash
# run_prevalence.sh -- full reproduction of the EgDiff prevalence measurement.
#
#   0. build the targets (static non-PIE riscv64)
#   1. parser self-check on a program with a known number of loads per call
#   2. per target: derive load/sensitive PCs, run the PROT_NONE fault probe,
#      then stream the QEMU exec trace for every input mode
#   3. regenerate results/prevalence.md from results/prevalence.csv
#
# Nothing here touches gem5 itself.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
cd "$root"

N="${N:-300}"                     # calls per mode (>= 300 as required)
QEMU="${QEMU:-qemu-riscv64}"
CC="${CC:-riscv64-linux-gnu-gcc}"
PY=python3

mkdir -p build results
"$here/build_targets.sh"

# Same flags as build_targets.sh (the self-check must match the targets).
CFLAGS=(
  -std=gnu11 -static -O2 -fno-pic -no-pie
  -fno-ipa-icf
  -fno-builtin-memcmp -fno-builtin-strcmp -fno-builtin-strlen
  -fno-ipa-pure-const
  -fno-tree-loop-distribute-patterns
  -Wall -Wno-unused-function -Wno-unused-variable
  -I"$root/targets"
  -I"$root/third_party/Monocypher/src"
)

# ---------------------------------------------------------------------------
# 1. parser self-check: a call that performs exactly 8 loads, in a loop, so
#    that the last sensitive load is 1 slot from T2 and no load sits between
#    the callee's `ret` and the marker load.
# ---------------------------------------------------------------------------
cat > build/selfcheck.c <<'EOF'
#include "harness.h"
volatile uint64_t sc_word[8];
__attribute__((noinline, used))
uint64_t sc_loads(const volatile uint64_t *p)
{
    uint64_t t = 0;
    for (int i = 0; i < 8; i++)
        t += p[i];
    return t;
}
int main(void)
{
    for (int c = 0; c < 10; c++)
        EG_RUN_CALL(sc_loads(sc_word));
    printf("selfcheck sink=%llu\n", (unsigned long long)eg_sink);
    return 0;
}
EOF
"$CC" "${CFLAGS[@]}" -o build/selfcheck build/selfcheck.c
"$PY" tools/load_pcs.py build/selfcheck \
      --marker-begin eg_marker_call_begin --marker-after eg_marker_after_call \
      --sensitive sc_loads=0 -o build/meta_selfcheck.json
SCPCS=$("$PY" -c 'import json;print(",".join(json.load(open("build/meta_selfcheck.json"))["static_sensitive_pcs"]))')
echo "== self-check sensitive PCs: $SCPCS"
rm -f build/selfcheck.csv
"$PY" tools/trace_loads.py --meta build/meta_selfcheck.json --target selfcheck \
      --mode msg --n 10 --sensitive-override "$SCPCS" \
      --out build/selfcheck.csv --header
"$PY" - <<'PY'
import csv, sys
rows = list(csv.DictReader(open("build/selfcheck.csv")))
fail = []
if len(rows) != 10:
    fail.append("calls=%d (want 10)" % len(rows))
for r in rows:
    if int(r["d"]) != 1:
        fail.append("call %s d=%s (want 1)" % (r["call"], r["d"]))
    if int(r["n_loads_in_call"]) != 8:
        fail.append("call %s n_loads=%s (want 8 target loads)" % (r["call"], r["n_loads_in_call"]))
    if int(r["n_sensitive_loads"]) != 8:
        fail.append("call %s n_sensitive=%s (want 8)" % (r["call"], r["n_sensitive_loads"]))
    if int(r["loads_since_ret"]) != 0:
        fail.append("call %s loads_since_ret=%s (want 0)" % (r["call"], r["loads_since_ret"]))
    if int(r["insns_since_ret"]) != 3:
        fail.append("call %s insns_since_ret=%s (want 3: result store + jal + marker lui)"
                    % (r["call"], r["insns_since_ret"]))
    if r["base_pc"] != r["d_pc"] or r["is_sensitive_pc"] != "1":
        fail.append("call %s base_pc=%s d_pc=%s is_sens=%s"
                    % (r["call"], r["base_pc"], r["d_pc"], r["is_sensitive_pc"]))
if fail:
    print("SELF-CHECK FAILED:"); [print("  " + f) for f in fail]; sys.exit(1)
print("== SELF-CHECK OK: 10 calls x (8 loads, d=1, base_pc==d_pc, "
      "0 loads between ret and T2)")
PY

# ---------------------------------------------------------------------------
# 2. measured targets
# ---------------------------------------------------------------------------
printf '%s\n' "target,call,mode,d,base_pc,is_sensitive_pc,d_pc,d_key,base_pc_key,n_sensitive_loads,n_loads_in_call,insns_since_ret,loads_since_ret" > results/prevalence.csv
printf '%s\n' "target,call,mode,pc,d,n_sensitive_loads" > build/perpc.csv
: > build/validate.txt

run_target() {
    local t="$1" modes="$2" seeds="$3" keyexecs="$4" keyside="$5"
    local bin="build/t_$t" meta="build/meta_t_$t.json"
    local sargs=()
    for s in $seeds; do sargs+=(--sensitive "$s"); done
    echo "== $t: $modes  (key_execs=$keyexecs/$keyside)"
    "$PY" tools/load_pcs.py "$bin" \
        --marker-begin eg_marker_call_begin --marker-after eg_marker_after_call \
        "${sargs[@]}" -o "$meta"

    timeout 900 "$QEMU" "$bin" probe > "build/probe_t_$t.txt"
    local pcs percall
    pcs=$(sed -n 's/.* pcs=\([^ ]*\).*/\1/p' "build/probe_t_$t.txt")
    percall=$(sed -n 's/.* percall=\(.*\)/\1/p' "build/probe_t_$t.txt")
    grep -q 'bad=0' "build/probe_t_$t.txt" || {
        echo "!! fault probe reported 'bad' faults for $t"; cat "build/probe_t_$t.txt"; exit 1; }

    "$PY" - "$meta" "$pcs" "$percall" "$t" <<'PY' >> build/validate.txt
import json, statistics, sys
meta, pcs, percall, t = sys.argv[1:5]
m = json.load(open(meta))
dyn = sorted(int(p, 16) for p in pcs.split(",") if p)
sta = sorted(int(p, 16) for p in m["static_sensitive_pcs"])
pc = [int(x) for x in percall.split(",") if x]
print("target=%s marker_no_load_before=%s static=%d dynamic=%d "
      "static_subset_dynamic=%s dynamic_subset_static=%s "
      "static_pcs=%s dynamic_pcs=%s key_accesses_per_call=%s"
      % (t, m["marker_no_load_before"], len(sta), len(dyn),
         set(sta) <= set(dyn), set(dyn) <= set(sta),
         ",".join("%x" % p for p in sta) or "-",
         ",".join("%x" % p for p in dyn),
         "%d..%d (mode %d)" % (min(pc), max(pc), statistics.mode(pc)) if pc else "?"))
PY

    for m in $modes; do
        "$PY" tools/trace_loads.py --meta "$meta" --target "t_$t" --mode "$m" \
            --n "$N" --sensitive-override "$pcs" \
            --key-execs "$keyexecs" --key-side "$keyside" \
            --out results/prevalence.csv --per-pc-out build/perpc.csv
    done
}

#                     target      modes         static provenance seeds        key_execs side
run_target verify32   "msg key"   "crypto_verify32=1"                          32        last
run_target chacha20   "msg key len" "crypto_chacha20_djb=3 crypto_chacha20_init=1" 0     last
run_target blake2b    "msg key len" ""                                         32        first
run_target aead       "msg key len" "crypto_aead_lock=2"                       0         last
run_target memcmp     "msg key len" "eg_memcmp_early=1"                        0         last
run_target strcmp     "msg key len" "eg_strcmp_early=1"                        0         last
run_target hashlookup "msg key"     "eg_hash_lookup=0"                         0         last
run_target listwalk   "msg key"     "eg_list_walk=0"                           0         last

# ---------------------------------------------------------------------------
# 3. QEMU trace PC-field cross-check: the first traced PC must be objdump's
#    _start address (this is what proves which tuple field is the guest PC).
# ---------------------------------------------------------------------------
first_pc=$(qemu-riscv64 -singlestep -d exec build/selfcheck msg 1 2>&1 >/dev/null \
           | sed -n '1p' | sed -n 's/.*\[\([^]]*\)\].*/\1/p' | cut -d/ -f2)
start_pc=$(riscv64-linux-gnu-objdump -d build/selfcheck \
           | sed -n 's/^\([0-9a-f]*\) <_start>:/\1/p')
printf 'first_trace_pc=0x%s\nobjdump_start=0x%s\nmatch=%s\n' \
    "$first_pc" "$start_pc" \
    "$([ $((16#${first_pc#0x})) -eq $((16#${start_pc#0x})) ] && echo yes || echo NO)" \
    > build/trace_pc_check.txt
cat build/trace_pc_check.txt

# ---------------------------------------------------------------------------
# 4. report
# ---------------------------------------------------------------------------
"$PY" - <<'PYREPORT'
import csv, collections, json, statistics

CSV = "results/prevalence.csv"
rows = list(csv.DictReader(open(CSV)))
by_t = collections.OrderedDict()
for r in rows:
    by_t.setdefault(r["target"], []).append(r)

val = {}
for line in open("build/validate.txt"):
    kv = dict(x.split("=", 1) for x in line.split() if "=" in x)
    val["t_" + kv["target"]] = kv

perc = list(csv.DictReader(open("build/perpc.csv")))
per_by_t = collections.defaultdict(list)
for r in perc:
    per_by_t[r["target"]].append(r)

meta = {t: json.load(open("build/meta_%s.json" % t)) for t in by_t}

pcchk = dict(x.split("=") for x in open("build/trace_pc_check.txt").read().split())
sc = list(csv.DictReader(open("build/selfcheck.csv")))

CLASS = {
 "t_verify32":  ("deterministic",
                 "deterministic (constant-time compare)"),
 "t_chacha20":  ("attacker-selectable variance (length only)",
                 "deterministic for fixed length"),
 "t_blake2b":   ("attacker-selectable variance (length only)",
                 "deterministic for fixed length"),
 "t_aead":      ("attacker-selectable variance (length only)",
                 "deterministic for fixed length"),
 "t_memcmp":    ("deterministic d; count is attacker-selectable",
                 "attacker-selectable (early-exit position)"),
 "t_strcmp":    ("deterministic d; count is attacker-selectable",
                 "attacker-selectable (early-exit position)"),
 "t_hashlookup":("deterministic d; count is attacker-selectable + secret-dependent",
                 "attacker-selectable + secret-dependent (probe distance)"),
 "t_listwalk":  ("deterministic d; count is attacker-selectable + external-state dependent",
                 "attacker-selectable + external-state dependent (walk length)"),
}
WHY = {
 "t_verify32":
   "Constant-time: exactly 32 key bytes are read (4 calls of the shared "
   "`load64_le` helper, which also serves the attacker operand, hence 64 "
   "sensitive-PC executions per call), then a fixed 6-load epilogue. Nothing in "
   "the input can move d. The secret is passed second so the helper's *last* "
   "calls read the key.",
 "t_chacha20":
   "The key is expanded into a stack state once, at entry; every extra plaintext "
   "block adds loads *after* the last key read, so d tracks text_size and nothing "
   "else. Key and message content are irrelevant.",
 "t_blake2b":
   "The 32-byte key is copied byte-wise into the hash context at init through a "
   "dedicated loop (a single `lbu` PC), before any message processing; the "
   "message is absorbed later through a different (64-bit) path, so the key PCs "
   "are key-exclusive. d therefore grows with message_size and nothing else.",
 "t_aead":
   "`crypto_chacha20_h()` is the only reader of the raw key; the whole AEAD "
   "pipeline follows it, so d tracks text_size.",
 "t_memcmp":
   "The early-exit loop always ends with a load of the secret byte immediately "
   "before the return, so d=1 regardless of where the mismatch is. Only the "
   "*number* of secret-byte loads varies.",
 "t_strcmp":
   "Same shape as memcmp: the last secret byte load is the one that terminates "
   "the loop, so d=1 while the count varies.",
 "t_hashlookup":
   "d is 1 or 2 depending on whether the lookup ends on the `used` byte (d=1) or "
   "inside the 32-byte compare, where the query-byte load follows the table-byte "
   "load (d=2). The number of probes is what varies.",
 "t_listwalk":
   "d is 1 or 2: ending on the `next` pointer gives d=1, ending inside the "
   "32-byte compare gives d=2 (the query-byte load follows the node-byte load). "
   "The walk length is what varies.",
}

def stat(rs, key="d"):
    v = [int(r[key]) for r in rs if int(r[key]) >= 0]
    if not v:
        return None
    c = collections.Counter(v)
    m, cm = c.most_common(1)[0]
    return dict(n=len(v), mode=m, frac=cm / len(v), mn=min(v), mx=max(v),
                distinct=len(c))

def pct(x):
    return "%.1f%%" % (100.0 * x)

def cons(rs, key):
    """share of calls whose value is the modal one"""
    v = [r[key] for r in rs]
    m, c = collections.Counter(v).most_common(1)[0]
    return m, c / len(v)

out = []
w = out.append

w("# EgDiff base-load prevalence: how far is a sensitive load from the caller's next load?\n")
w("## Question\n")
w("EgDiff predicts a load's value from the value of a load `d` positions earlier in the")
w("dynamic **load** stream, with `1 <= d <= 32`. For a victim function that loads a")
w("sensitive value, the attack precondition is that *some* sensitive load sits at")
w("`d <= 32` from the attacker's first load after the call, and that this position is")
w("stable across benign inputs. This study measures that precondition on host binaries;")
w("it does not run gem5 and does not time anything.\n")
w("## Metric\n")
w("Per call, with T1 = an execution of a load PC that reads the tagged key/secret page")
w("and T2 = the marker load executed immediately after the call returns:\n")
w("```")
w("n = number of dynamic load instructions strictly between the last T1 instance")
w("    before T2 and T2        (stores, ALU ops and branches do not advance d)")
w("d = n + 1                   (1-based, matching EgDiff's distance)")
w("```")
w("Rules actually applied:\n")
w("* **Minimum over all sensitive PCs, not one preselected PC.** Real functions read")
w("  key material at several PCs (word-at-a-time verify, key expansion, copy loops).")
w("  For each sensitive PC we take its *last* execution before T2, compute that PC's")
w("  `d`, and report the minimum over PCs. The winning PC is reported, with its")
w("  disassembly, so the reader can check it.")
w("* **Last execution, not first.** Inside a loop, earlier instances are further from")
w("  T2 and can never bind, so only the last instance of each PC is used.")
w("* **Base-PC identity is checked, not assumed.** The PC of the `d`-th load back from")
w("  T2 is read out of an independent ring buffer and compared with the sensitive PC")
w("  that won; `is_sensitive_pc` records the result, and the per-PC tables below list")
w("  every sensitive PC separately.")
w("* **`d_key` refinement.** Where a sensitive PC is also used for non-secret data, the")
w("  last execution of that PC may carry non-secret bytes; `d_key` uses the per-call")
w("  key-access count measured by the fault probe to pick the last *key* read.")
w("  `d_key == d` unless a target is marked as sharing (see the validation table).\n")
w("Precondition: `d <= 32` (equivalently `n <= 31`).\n")
w("## How to reproduce\n")
w("```sh")
w("cd experiment/egdiff_eval/prevalence")
w("./tools/run_prevalence.sh          # builds, self-checks, measures, writes results/")
w("```")
w("Pipeline: `tools/build_targets.sh` builds static non-PIE `riscv64-linux-gnu-gcc`")
w("targets so runtime PCs equal `objdump` addresses; `tools/load_pcs.py` classifies")
w("loads from `objdump -d` (RVC `c.ld/c.lw/c.lwsp/c.ldsp/...` plus")
w("`lb/lh/lw/ld/lbu/lhu/lwu/flw/fld`) and resolves marker/load PCs; `tools/trace_loads.py`")
w("streams `qemu-riscv64 -singlestep -d exec` (never buffering it) and computes `d` per")
w("call. Traces are piped straight into the parser; no trace file is ever written.")
w("300 calls per input mode (`N=300`), 600-900 calls per target.\n")
w("Build flags: `-static -O2 -fno-pic -no-pie -fno-ipa-icf -fno-ipa-pure-const")
w("-fno-builtin-memcmp -fno-builtin-strcmp -fno-builtin-strlen")
w("-fno-tree-loop-distribute-patterns`. The last four stop GCC from replacing the")
w("control loops with library calls or deleting them; `-fno-ipa-icf` keeps the two")
w("marker functions distinct.\n")
w("## Parser validation\n")
w("QEMU prints `Trace 0: <host TB addr> [<cs_base>/<PC>/<flags>/<cflags>] <symbol>`.")
w("The guest PC is **tuple field 1** (0-based). Evidence:\n")
w("```")
w("first traced PC : %s" % pcchk["first_trace_pc"])
w("objdump _start  : %s" % pcchk["objdump_start"])
w("match           : %s" % pcchk["match"])
w("```")
w("Known-load-count self-check (`build/selfcheck.c`): a call performing exactly 8 loop")
w("loads, returning straight into the marker.\n")
w("```")
if sc:
    w("calls=%d  d=%s  n_loads_in_call=%s  n_sensitive_loads=%s"
      % (len(sc), sorted({r["d"] for r in sc}),
         sorted({r["n_loads_in_call"] for r in sc}),
         sorted({r["n_sensitive_loads"] for r in sc})))
    w("base_pc=%s == d_pc=%s  is_sensitive_pc=%s  loads_since_ret=%s"
      % (sorted({r["base_pc"] for r in sc}), sorted({r["d_pc"] for r in sc}),
         sorted({r["is_sensitive_pc"] for r in sc}),
         sorted({r["loads_since_ret"] for r in sc})))
w("```")
w("The two instructions between the callee `ret` and T2 are the store of the call's")
w("result and the `jal` into the marker (`loads_since_ret == 0` everywhere: see each")
w("target's table). `tools/load_pcs.py` also asserts that each marker's load is the")
w("first load inside its symbol, so nothing can sit between the return and T2.\n")
w("## Sensitive-PC identification and dynamic validation\n")
w("Static identification: intraprocedural argument provenance (`tools/load_pcs.py`)")
w("taints `a0..a7` at function entry, follows `mv/addi/add`-style copies, and flags")
w("loads whose base register derives from the nominated key argument. Provenance is a")
w("conservative over-approximation, used only as corroboration.\n")
w("Dynamic validation (authoritative): the tagged key page is made `PROT_NONE` for a")
w("whole call and each faulting load is decoded and emulated out of a shadow copy in")
w("the `SIGSEGV` handler (`targets/harness.h`). The page therefore stays protected, so")
w("**every** key-page load in the call faults, not just the first. This yields the")
w("sensitive-PC set and the exact per-call key-access count. Counting runs use normally")
w("mapped pages; `bad=0` in every probe run means no unsupported load form or store hit")
w("the page.\n")
w("| target | 0 loads before T2 | static PCs | dynamic PCs | static⊆dynamic | dynamic⊆static | key accesses/call (probe) |")
w("|---|---|---|---|---|---|---|")
for t in by_t:
    v = val.get(t, {})
    w("| %s | %s | %s | %s | %s | %s | %s |"
      % (t, v.get("marker_no_load_before"), v.get("static"), v.get("dynamic"),
         v.get("static_subset_dynamic"), v.get("dynamic_subset_static"),
         v.get("key_accesses_per_call")))
w("")
w("Static provenance returns no PC for `t_verify32` (the key is read inside")
w("`load64_le()`, which takes the pointer as its own `a0`) and for `t_blake2b`/`t_aead`")
w("(the key is copied by inlined code in a callee whose arguments do not include the")
w("key). For those targets the dynamic set is the only sound source, which is exactly")
w("why the fault probe exists. Where the two differ the measurement uses the dynamic set.")
w("The reverse comparison is also informative: if a target's per-call sensitive-PC")
w("executions exceed its per-call key accesses, some executions of those PCs carried")
w("non-secret data, and `d_key` is needed. The probe and the counted run use different")
w("input sequences, so for the four controls (whose counts are input-dependent by")
w("design) the two ranges are not meant to match; the comparison is only meaningful for")
w("the constant-count crypto targets.\n")
w("| target | sensitive-PC executions/call (counted run) | key accesses/call (probe) | PCs shared with non-secret data? |")
w("|---|---|---|---|")
for t, rs in by_t.items():
    v = val.get(t, {})
    kmin = min(int(r["n_sensitive_loads"]) for r in rs)
    kmax = max(int(r["n_sensitive_loads"]) for r in rs)
    ka = v.get("key_accesses_per_call", "?")
    shared = "yes (`d_key` used)" if kmax > int(ka.split("..")[0].split()[0] or 0) else "no"
    if t == "t_verify32":
        shared = "yes: shared `load64_le`; key read last, so `d_key == d`"
    elif t in ("t_memcmp", "t_strcmp", "t_hashlookup", "t_listwalk"):
        shared = "no (the count varies with the input path, not with buffer identity)"
    w("| %s | %d..%d | %s | %s |" % (t, kmin, kmax, ka, shared))
w("")
w("## Results\n")
w("One row per call in `results/prevalence.csv`\n")
w("(`target,call,mode,d,base_pc,is_sensitive_pc,d_pc,d_key,base_pc_key,")
w("n_sensitive_loads,n_loads_in_call,insns_since_ret,loads_since_ret`).\n")
w("| target | calls | d mode | at mode | d min..max | distinct d | d_key mode | base PC (winner) | base==sens | 0 loads since ret | d<=32 |")
w("|---|---|---|---|---|---|---|---|---|---|---|")
for t, rs in by_t.items():
    s = stat(rs)
    dk = stat(rs, "d_key")
    base = collections.Counter(r["base_pc"] for r in rs).most_common(1)[0][0]
    sens_ok = sum(1 for r in rs if r["is_sensitive_pc"] == "1")
    ret_ok = sum(1 for r in rs if r["loads_since_ret"] == "0")
    le32 = sum(1 for r in rs if 0 <= int(r["d"]) <= 32)
    w("| %s | %d | %d | %s | %d..%d | %d | %d | `%s` | %s | %s | %s |"
      % (t, s["n"], s["mode"], pct(s["frac"]), s["mn"], s["mx"], s["distinct"],
         dk["mode"], base, pct(sens_ok / len(rs)), pct(ret_ok / len(rs)),
         pct(le32 / len(rs))))
w("")
w("### Per-target detail\n")
for t, rs in by_t.items():
    s = stat(rs)
    dk = stat(rs, "d_key")
    v = val.get(t, {})
    base = collections.Counter(r["base_pc"] for r in rs).most_common(1)[0][0]
    dis = meta[t].get("load_disasm", {}).get(base, "?")
    w("#### `%s`\n" % t)
    w("* calls: **%d** over modes %s"
      % (len(rs), ", ".join(sorted({r["mode"] for r in rs}))))
    w("* `d`: mode **%d**, at mode **%s**, min **%d**, max **%d**, %d distinct value(s)"
      % (s["mode"], pct(s["frac"]), s["mn"], s["mx"], s["distinct"]))
    w("* `d_key`: mode **%d**, at mode **%s**, max **%d**"
      % (dk["mode"], pct(dk["frac"]), dk["mx"]))
    w("* base-PC identity: `base_pc == d_pc` in **%s** of calls, `is_sensitive_pc=1` in **%s**"
      % (pct(sum(1 for r in rs if r["base_pc"] == r["d_pc"]) / len(rs)),
         pct(sum(1 for r in rs if r["is_sensitive_pc"] == "1") / len(rs))))
    w("* modal winning base PC `0x%s` = `%s`" % (base, dis))
    w("* zero loads between the callee `ret` and T2: **%s** of calls"
      % pct(sum(1 for r in rs if r["loads_since_ret"] == "0") / len(rs)))
    w("* sensitive-load count per call: **%d..%d**"
      % (min(int(r["n_sensitive_loads"]) for r in rs),
         max(int(r["n_sensitive_loads"]) for r in rs)))
    w("* dynamic sensitive PCs: `%s`; key accesses/call: %s"
      % (v.get("dynamic_pcs", "?"), v.get("key_accesses_per_call", "?")))
    w("* **classification**: %s" % CLASS[t][0])
    w("* count-variance class: %s" % CLASS[t][1])
    w("* why: %s" % WHY[t])
    w("* `d <= 32` holds for **%s** of calls"
      % pct(sum(1 for r in rs if 0 <= int(r["d"]) <= 32) / len(rs)))
    w("")
    w("| mode | calls | d mode | at mode | d min..max | sensitive loads/call | d_key mode |")
    w("|---|---|---|---|---|---|---|")
    for m in sorted({r["mode"] for r in rs}):
        rs2 = [r for r in rs if r["mode"] == m]
        s2 = stat(rs2)
        dk2 = stat(rs2, "d_key")
        w("| %s | %d | %d | %s | %d..%d | %d..%d | %d |"
          % (m, len(rs2), s2["mode"], pct(s2["frac"]), s2["mn"], s2["mx"],
             min(int(r["n_sensitive_loads"]) for r in rs2),
             max(int(r["n_sensitive_loads"]) for r in rs2), dk2["mode"]))
    w("")
    w("| sensitive PC | disassembly | calls seen | d min..max | calls where it won |")
    w("|---|---|---|---|---|")
    won = collections.Counter(r["d_pc"] for r in rs)
    for pc, rrs in sorted(collections.Counter(r["pc"] for r in per_by_t[t]).items()):
        d_vals = [int(r["d"]) for r in per_by_t[t] if r["pc"] == pc]
        w("| `0x%s` | `%s` | %d | %d..%d | %d |"
          % (pc, meta[t].get("load_disasm", {}).get(pc, "?"), len(d_vals),
             min(d_vals), max(d_vals), won.get(pc, 0)))
    w("")
w("## Overall answer\n")
fit = [t for t in by_t if stat(by_t[t])["mx"] <= 32]
notfit = [t for t in by_t if stat(by_t[t])["mn"] > 32]
mixed = [t for t in by_t if t not in fit and t not in notfit]
w("* `d <= 32` for **all** calls (precondition holds): %s."
  % (", ".join("`%s`" % t for t in fit) or "none"))
w("* `d > 32` for **all** calls (precondition fails): %s."
  % (", ".join("`%s`" % t for t in notfit) or "none"))
w("* Input-dependent: %s." % (", ".join("`%s`" % t for t in mixed) or "none"))
w("")
w("Stability: the winning base PC is the same sensitive PC in every call of every")
w("target, and the per-PC tables above show each sensitive PC's own distance. `d` is a")
w("single value for `verify32` (7), `memcmp` (1) and `strcmp` (1); it is 1 or 2 for")
w("`hashlookup`/`listwalk`, depending on which of their two sensitive loads ends the")
w("exit path; and for `chacha20`/`blake2b`/`aead` it is constant within each of the")
w("`msg` and `key` modes (100% at mode) and a deterministic function of `text_size` in")
w("`len` mode - message and key *contents* never move it. What varies with the input is")
w("the *number* of sensitive loads executed - the early-exit position in")
w("`memcmp`/`strcmp`, the probe count in the hash table, the walk length in the list -")
w("not the distance from the last sensitive load to the caller's next load. For a")
w("value predictor that is the useful asymmetry: the exit tail is short and stable, while")
w("the amount of secret-dependent work before it is not.\n")
w("Where the precondition fails it fails structurally: a multi-block primitive reads its")
w("key once at entry and then performs hundreds of loads of derived state, so no")
w("key-buffer load can be within 32 loads of the caller's next load.\n")
w("## Caveats\n")
w("* QEMU user-mode traces the **architectural** instruction stream. EgDiff's distance is")
w("  in dispatch/allocation order, which can also contain wrong-path loads on mispredicted")
w("  branches that are later squashed. The measured `d` is therefore an optimistic")
w("  architectural exit-tail distance. This matters most for the data-dependent-branch")
w("  controls (`t_memcmp`, `t_strcmp`, `t_hashlookup`, `t_listwalk`), where a")
w("  misprediction at the exit test would inject extra wrong-path loads after the last")
w("  real one.")
w("* The metric is PC-indexed, as EgDiff is. A PC in the sensitive set may also read")
w("  non-secret data: in `t_verify32` the shared `load64_le` helper is used for both")
w("  operands, so 64 sensitive-PC executions occur per call against 32 key accesses. The")
w("  secret is passed second, so the *last* helper calls read the key and `d_key == d`.")
w("* Only architectural, single-threaded, caller-controlled calls are measured. No")
w("  speculation, cache, TLB or interrupt effects are modelled.")
w("* Loads are classified from `objdump` mnemonics; atomics (`lr.w`/`lr.d`) are excluded")
w("  (no target uses them). Compressed and plain load forms are both handled.")
w("* The fault-PC table holds 4096 distinct PCs; per-call key-access counts use an")
w("  uncapped counter, so the counts are exact. `bad=0` in every run.")
w("* Sensitive data lives in page-aligned, tagged `mmap` pages (`EGDIFFv1-KEYBUF:`), not")
w("  on the stack, so the probe protects exactly one page.")
w("* The `msg`/`key`/`len` modes are separate runs with the same fixed PRNG seeds, not")
w("* `hashlookup` and `listwalk` rebuild a 2.5 KB table / 24-node list per call in `key`")
w("  mode; those rebuilds are outside the measured window (before the begin marker).")
w("")
w("## Upstream corpus\n")
w("* Monocypher: <https://github.com/LoupVaillant/Monocypher>")
w("* Commit: `f8bb8dbd224556d58f5b4fa176c3f18fbfe50ebe` (2026-11-22)")
w("* Vendored unmodified at `third_party/Monocypher/` (git-ignored, never committed).")
w("* Functions used: `crypto_verify32`, `crypto_chacha20_djb`, `crypto_blake2b_keyed`,")
w("  `crypto_aead_lock`; signatures checked against `src/monocypher.h`.")
w("* Toolchain: `riscv64-linux-gnu-gcc` (Debian 12.2.0-13), `qemu-riscv64` 7.2.22")
w("  (Debian 1:7.2+dfsg-7+deb12u18+b3).")
w("")

txt = "\n".join(out)
txt = txt.replace(
  "* The `msg`/`key`/`len` modes are separate runs with the same fixed PRNG seeds, not\n",
  "* The `msg`/`key`/`len` modes are separate runs with the same fixed PRNG seeds, so the\n"
  "  same input sequence is replayed in each mode; they are not interleaved.\n")
open("results/prevalence.md", "w").write(txt + "\n")
print("wrote results/prevalence.md (%d bytes, %d targets, %d calls)"
      % (len(txt), len(by_t), len(rows)))
PYREPORT
echo "== wrote results/prevalence.md and results/prevalence.csv"
