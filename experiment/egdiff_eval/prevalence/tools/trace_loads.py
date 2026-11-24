#!/usr/bin/env python3
"""trace_loads.py -- measure EgDiff load distance d from a QEMU exec trace.

Method
------
Run `qemu-riscv64 -singlestep -d exec` on a target binary and stream the trace
(never buffering it).  QEMU prints one line per executed instruction:

    Trace 0: <host TB addr> [<cs_base>/<GUEST PC>/<flags>/<cflags>] <symbol>

The guest PC is field 1 of the bracketed tuple (0-based); this is validated
against objdump on the `_start` address of a known binary.

The dynamic *load* stream is the PC stream intersected with the set of load PCs
from objdump (tools/load_pcs.py, RVC forms included).  The harness emits a
marker load before each call (`marker_begin_pc`) and a marker load immediately
after the call returns (`marker_after_pc` == T2).

Metric (per call, per sensitive load PC s): take the LAST execution of s before
T2 (earlier executions of a looped PC are further away, so they never bind) and

        d(s) = index(T2) - index(last execution of s)  =  n + 1

where n is the number of dynamic loads strictly between that sensitive load and
T2.  The reported `d` is the MINIMUM over all sensitive PCs: the question is
whether *any* sensitive load sits inside the 32-entry window.

`d_key` is a refinement for targets whose sensitive PCs are also used for
non-secret data (a byte-load helper serving both operands, or a hash routine
absorbing both key and message).  There, the last execution of a sensitive PC
may carry non-secret bytes.  `--key-execs N` (the per-call key-page access
count measured by the PROT_NONE probe) plus `--key-side first|last` selects the
execution that is the last *key* read:

    first   the key is absorbed before the non-secret data   (blake2b)
    last    the key is read last, or every sensitive PC execution is a key
            read, so the last sensitive-PC execution already is the last key
            read and `d_key == d`                                (default)

With `--key-execs 0` or `--key-side last`, `d_key == d`.
"""
import argparse
import collections
import json
import subprocess
import sys

COLS = ["target", "call", "mode", "d", "base_pc", "is_sensitive_pc", "d_pc",
        "d_key", "base_pc_key", "n_sensitive_loads", "n_loads_in_call",
        "insns_since_ret", "loads_since_ret"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--meta", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("--mode", required=True)
    ap.add_argument("--qemu", default="qemu-riscv64")
    ap.add_argument("--n", type=int, default=300)
    ap.add_argument("--out", default="-")
    ap.add_argument("--header", action="store_true")
    ap.add_argument("--count-only", action="store_true",
                    help="just report total instructions/loads (self-check)")
    ap.add_argument("--sensitive-override", default=None,
                    help="comma-separated hex PCs from the fault probe")
    ap.add_argument("--key-execs", type=int, default=0,
                    help="key-page accesses per call (0 = all)")
    ap.add_argument("--key-side", choices=["first", "last"], default="last")
    ap.add_argument("--per-pc-out", default=None,
                    help="append per-sensitive-PC distances to this file")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    meta = json.load(open(args.meta))
    binary = meta["binary"]
    marker_begin = meta["marker_begin_pc"]
    marker_after = meta["marker_after_pc"]
    ret_pcs = {int(p, 16) for p in meta["ret_pcs"]}
    load_pcs = {int(p, 16) for p in meta["load_pcs"]}

    if args.sensitive_override:
        sens = {int(p, 16) for p in args.sensitive_override.split(",") if p}
    else:
        sens = {int(p, 16) for p in meta.get("probe_sensitive_pcs", [])}
    if not sens:
        sys.stderr.write("error: no sensitive PCs in %s (run the probe first)\n"
                         % args.meta)
        return 2

    cmd = [args.qemu, "-singlestep", "-d", "exec", binary, args.mode,
           str(args.n)]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True,
                            errors="replace", bufsize=1)

    ring = collections.deque(maxlen=8192)     # (load_index, pc)
    last_exec = {}                            # sensitive pc -> last load index
    load_idx = -1
    in_call = False
    n_sens = 0
    n_loads = 0
    sens_idx_kth = None
    prev_pc = None
    prev_was_ret = False
    insns_since_ret = 0
    loads_since_ret = 0
    call = 0
    n_insn = 0
    n_ring_mismatch = 0
    rows = []
    per_pc = []

    for line in proc.stderr:
        n_insn += 1
        i = line.find("[")
        if i < 0:
            continue
        j = line.find("]", i)
        pc = int(line[i + 1:j].split("/")[1], 16)
        is_load = pc in load_pcs

        if prev_was_ret:
            insns_since_ret = 0
            loads_since_ret = 0
            prev_was_ret = False

        # T2 has not been pushed onto the ring yet, so load_idx is the index of
        # the load just before T2 and d = (load_idx + 1) - idx is exactly n+1.
        if is_load and in_call and pc == marker_after:
            d_best = d_pc = base_pc = best_idx = None
            for spc, idx in last_exec.items():
                d = (load_idx + 1) - idx
                if d_best is None or d < d_best:
                    d_best, d_pc, best_idx = d, spc, idx
            if d_best is not None and d_best <= len(ring):
                base_idx, base_pc = ring[-d_best]
                if base_idx != best_idx or base_pc != d_pc:
                    n_ring_mismatch += 1
            if args.per_pc_out:
                for spc, idx in last_exec.items():
                    per_pc.append((args.target, call, args.mode, "%x" % spc,
                                   (load_idx + 1) - idx, n_sens))
            dk, basek = d_best, base_pc
            # `first` only: the key reads are the *first* K sensitive-PC
            # executions, so the last key read is the K-th one.  With `last`
            # (or K == 0) the last sensitive-PC execution is already the last
            # key read, so d_key == d.
            if args.key_execs and args.key_side == "first" and sens_idx_kth is not None:
                dk = (load_idx + 1) - sens_idx_kth
                basek = ring[-dk][1] if dk <= len(ring) else None
            rows.append((
                args.target, call, args.mode,
                -1 if d_best is None else d_best,
                "-" if base_pc is None else "%x" % base_pc,
                "-" if base_pc is None else int(base_pc in sens),
                "-" if d_pc is None else "%x" % d_pc,
                -1 if dk is None else dk,
                "-" if basek is None else "%x" % basek,
                n_sens, n_loads, insns_since_ret, loads_since_ret,
            ))
            call += 1
            in_call = False
            sens_idx_kth = None

        if is_load:
            load_idx += 1
            ring.append((load_idx, pc))
            if pc == marker_begin:
                in_call = True
                last_exec.clear()
                n_sens = 0
                n_loads = 0
                sens_idx_kth = None
                insns_since_ret = 0
                loads_since_ret = 0
            elif in_call:
                n_loads += 1
                if pc in sens:
                    last_exec[pc] = load_idx
                    n_sens += 1
                    if (args.key_execs and args.key_side == "first"
                            and n_sens == args.key_execs):
                        sens_idx_kth = load_idx

        insns_since_ret += 1
        if is_load:
            loads_since_ret += 1
        prev_was_ret = pc in ret_pcs
        prev_pc = pc

    proc.wait()
    guest_out = proc.stdout.read() if proc.stdout else ""

    if args.count_only:
        print("insns=%d loads=%d calls=%d" % (n_insn, load_idx + 1, call))
        return 0

    if args.out == "-":
        print(",".join(COLS))
        for r in rows:
            print(",".join(str(x) for x in r))
    else:
        with open(args.out, "a") as f:
            if args.header:
                f.write(",".join(COLS) + "\n")
            for r in rows:
                f.write(",".join(str(x) for x in r) + "\n")
    if args.per_pc_out:
        with open(args.per_pc_out, "a") as f:
            for r in per_pc:
                f.write(",".join(str(x) for x in r) + "\n")

    if call != args.n:
        sys.stderr.write("warning: %s/%s produced %d calls, expected %d "
                         "(guest stdout: %s)\n"
                         % (args.target, args.mode, call, args.n,
                            guest_out.strip()))
        if call < args.n:
            return 1
    if not args.quiet:
        sys.stderr.write("%s/%s: %d calls, %d insns, %d loads, ring_mismatch=%d\n"
                         % (args.target, args.mode, call, n_insn,
                            load_idx + 1, n_ring_mismatch))
    return 0


if __name__ == "__main__":
    sys.exit(main())
