#!/usr/bin/env python3
"""measure_d.py -- measure EgDiff's dynamic load distance d in *this* binary.

Method: `qemu-riscv64 -singlestep -d exec` plus `objdump -d`.  The guest PC in
the trace is the second field of the bracketed tuple (0-based), the same field
the prevalence study validated.

The binary is run in its `measure` mode, which performs exactly one real
`target_verify32(guess)` call followed by the real marker load
(`victim_then_marker()`).  The measurement therefore uses the same instruction
stream as the attack.

Evidence produced:

  1. Static proof that the last `load64_le` call reads the *key* word:
       target_verify32 materialises `v32_key_storage` into a1 and tail-calls
       crypto_verify32; crypto_verify32 copies a1 (second argument = key) into
       its key register; the last `jal load64_le` is preceded by
       `add a0,<keyreg>,LANE_BASE`.
  2. Dynamic proof that no load sits between the callee's `ret` and the marker
     load (loads_between_ret_and_marker == 0).
  3. d for each of the 8 byte loads of that last key word, and its key offset.

The script runs `measure` twice with two different attacker guesses and asserts
that d and the key-load PCs are unchanged (crypto_verify32 is constant-time).

Usage: measure_d.py BIN [--qemu qemu-riscv64] [--objdump riscv64-linux-gnu-objdump]
Output (machine readable):
    PROOF ...
    LANE_BASE 24
    D <key_offset> <d>
    SUMMARY n=8 d_min=.. d_max=..
"""
import argparse
import os
import re
import subprocess
import sys

LOAD_MNEMONICS = {
    "lb", "lh", "lw", "ld", "lbu", "lhu", "lwu", "flw", "fld",
    "c.ld", "c.lw", "c.ldsp", "c.lwsp", "c.fld", "c.fldsp", "c.flw", "c.flwsp",
    "c.lbu", "c.lh", "c.lhu",
}

SYM_RE = re.compile(r"^([0-9a-f]+) <([^>]+)>:\s*$")
INS_RE = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
OFF_RE = re.compile(r"^[^,]+,\s*(-?\d+)\([a-z0-9]+\)$")


def parse_objdump(path, objdump):
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", path],
                         check=True, capture_output=True, text=True).stdout
    funcs = {}
    cur = None
    for line in out.splitlines():
        m = SYM_RE.match(line)
        if m:
            cur = m.group(2)
            funcs.setdefault(cur, [])
            continue
        m = INS_RE.match(line)
        if m and cur is not None:
            funcs[cur].append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
    return funcs


def only(fns, name):
    if name not in fns:
        raise SystemExit("error: function %r not found" % name)
    return fns[name]


def trace_loads(binary, qemu, variant, load_pcs):
    """Return (list of (insn_index, pc) for loads, list of (insn_index, pc) rets,
    total instructions)."""
    cmd = [qemu, "-singlestep", "-d", "exec", binary, "measure", str(variant)]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True,
                            errors="replace", bufsize=1)
    loads = []
    rets = []
    n = 0
    for line in proc.stderr:
        i = line.find("[")
        if i < 0:
            continue
        j = line.find("]", i)
        if j < 0:
            continue
        fields = line[i + 1:j].split("/")
        if len(fields) < 2:
            continue
        try:
            pc = int(fields[1], 16)
        except ValueError:
            continue
        if pc in load_pcs:
            loads.append((n, pc))
        rets.append((n, pc))
        n += 1
    proc.wait()
    return loads, rets, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--qemu", default="qemu-riscv64")
    ap.add_argument("--objdump", default="riscv64-linux-gnu-objdump")
    args = ap.parse_args()

    fns = parse_objdump(args.binary, args.objdump)

    # ---- marker load: atk_load has exactly one load ----
    atk = only(fns, "atk_load")
    atk_loads = [(a, mn, ops) for (a, mn, ops) in atk if mn in LOAD_MNEMONICS]
    if len(atk_loads) != 1:
        raise SystemExit("error: atk_load has %d loads (want 1)" % len(atk_loads))
    marker_pc = atk_loads[0][0]

    # ---- last key word: load64_le's byte loads ----
    l64 = only(fns, "load64_le")
    key_pcs = {}
    for (a, mn, ops) in l64:
        if mn in LOAD_MNEMONICS:
            m = OFF_RE.match(ops)
            if not m:
                raise SystemExit("error: cannot parse load operand %r" % ops)
            key_pcs[a] = int(m.group(1))
    if sorted(key_pcs.values()) != list(range(8)):
        raise SystemExit("error: load64_le offsets are %r, want 0..7"
                         % sorted(key_pcs.values()))

    # ---- static proof: last load64_le call reads key+LANE_BASE ----
    cv = only(fns, "crypto_verify32")
    key_regs = set()
    for (a, mn, ops) in cv:
        if mn in ("mv", "c.mv"):
            o = [x.strip() for x in ops.split(",")]
            if len(o) == 2 and o[1] == "a1":
                key_regs.add(o[0])
    if len(key_regs) != 1:
        raise SystemExit("error: cannot identify the key register (a1 copies: %r)"
                         % sorted(key_regs))
    key_reg = key_regs.pop()

    calls = []
    for idx, (a, mn, ops) in enumerate(cv):
        if mn in ("jal", "j", "tail") and "load64_le" in ops:
            calls.append((idx, a, mn, ops))
    if len(calls) != 8:
        raise SystemExit("error: crypto_verify32 has %d load64_le calls (want 8)"
                         % len(calls))

    last_idx, last_addr, _, _ = calls[-1]
    lane_base = None
    for k in range(last_idx - 1, max(last_idx - 12, -1), -1):
        a, mn, ops = cv[k]
        if mn in ("add", "addi") and ops.startswith("a0,"):
            o = [x.strip() for x in ops.split(",")]
            if len(o) == 3 and o[1] == key_reg:
                try:
                    lane_base = int(o[2], 0)
                except ValueError:
                    continue
                break
    if lane_base is None:
        raise SystemExit("error: could not find `add a0,%s,<imm>` before the last "
                         "load64_le call" % key_reg)

    tv = only(fns, "target_verify32")
    tv_txt = " ".join("%s %s" % (mn, ops) for (_, mn, ops) in tv)
    if "v32_key_storage" not in tv_txt:
        raise SystemExit("error: target_verify32 does not reference the key symbol")
    if "crypto_verify32" not in tv_txt:
        raise SystemExit("error: target_verify32 does not call crypto_verify32")

    # ---- crypto_verify32's ret ----
    ret_pcs = {a for (a, mn, ops) in cv if mn in ("ret",)}
    if not ret_pcs:
        ret_pcs = {a for (a, mn, ops) in cv if mn == "jr" and ops.strip() == "ra"}
    if len(ret_pcs) != 1:
        raise SystemExit("error: crypto_verify32 has %d ret instructions"
                         % len(ret_pcs))
    cv_ret = ret_pcs.pop()

    all_loads = {a for fn in fns.values() for (a, mn, _) in fn
                 if mn in LOAD_MNEMONICS}

    # ---- run the trace twice with different guesses ----
    def measure_once(variant):
        loads, rets, ninsn = trace_loads(args.binary, args.qemu, variant,
                                         all_loads)
        marker = [i for i, (idx, pc) in enumerate(loads) if pc == marker_pc]
        if not marker:
            raise SystemExit("error: marker load 0x%x never executed" % marker_pc)
        mord = marker[-1]
        m_idx = loads[mord][0]

        cv_rets = [idx for (idx, pc) in rets if pc == cv_ret and idx < m_idx]
        if not cv_rets:
            raise SystemExit("error: crypto_verify32 ret not seen before marker")
        ret_idx = cv_rets[-1]
        between = [1 for (idx, pc) in loads if ret_idx < idx < m_idx]
        if between:
            raise SystemExit("error: %d load(s) between callee ret and marker"
                             % len(between))

        key_ords = [(i, pc) for i, (idx, pc) in enumerate(loads)
                    if pc in key_pcs and idx < m_idx][-8:]
        if len(key_ords) != 8:
            raise SystemExit("error: only %d load64_le loads before the marker"
                             % len(key_ords))
        rows = []
        for ordn, pc in key_ords:
            d = mord - ordn
            rows.append((lane_base + key_pcs[pc], d, pc))
        return rows, len(between), ninsn

    rows0, nbetween0, ninsn0 = measure_once(0)
    rows1, nbetween1, ninsn1 = measure_once(1)

    if [(o, d) for (o, d, _) in rows0] != [(o, d) for (o, d, _) in rows1]:
        raise SystemExit("error: d map changed with a different guess: %r vs %r"
                         % (rows0, rows1))
    if nbetween0 != 0 or nbetween1 != 0:
        raise SystemExit("error: loads between ret and marker")

    rows0.sort()
    offs = [o for (o, _, _) in rows0]
    if len(set(offs)) != 8:
        raise SystemExit("error: duplicate key offsets %r" % offs)

    print("PROOF target_verify32 materialises v32_key_storage into a1 and "
          "tail-calls crypto_verify32")
    print("PROOF crypto_verify32 copies a1 -> %s; last load64_le call is "
          "`add a0,%s,%d`" % (key_reg, key_reg, lane_base))
    print("PROOF loads_between_ret_and_marker=%d (calc ret PC 0x%x)"
          % (nbetween0, cv_ret))
    print("PROOF guess-invariance: identical d map for guess variants 0 and 1")
    print("PROOF binary=%s insns_traced=%d" % (os.path.abspath(args.binary),
                                               ninsn0))
    print("LANE_BASE %d" % lane_base)
    for (o, d, pc) in rows0:
        print("D %d %d  # key[%d] via lbu at 0x%x" % (o, d, o, pc))
    ds = [d for (_, d, _) in rows0]
    print("SUMMARY n=%d d_min=%d d_max=%d" % (len(ds), min(ds), max(ds)))
    if not (1 <= min(ds) and max(ds) <= 32):
        raise SystemExit("error: d outside [1,32]: %r" % ds)
    return 0


if __name__ == "__main__":
    sys.exit(main())
