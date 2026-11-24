#!/usr/bin/env python3
"""
Compare the architectural load distance with the dispatch-order distance.

    d = 1 + (number of dynamic loads strictly between the last execution of a
             sensitive load PC and the marker load)

"Architectural" comes from `qemu-riscv64 -singlestep -d exec`; "dispatch" comes
from a gem5 run with `--debug-flags=EgDiff`, whose `alloc sn=.. pc=..` lines are
exactly the speculative-queue allocation order (what EgDiff's distance indexes).

The probe is assumed to make ONE call followed by the marker (memcmp_probe.c).

Usage:
  measure_dispatch_d.py BIN --sensitive 0xPC[,0xPC...]
                           [--marker-sym atk_load]
                           [--qemu-log FILE] [--gem5-log FILE]

If a log is omitted the corresponding measurement is skipped.
"""
import argparse
import re
import subprocess

LOADS = {
    "lb", "lh", "lw", "ld", "lbu", "lhu", "lwu", "flw", "fld",
    "c.ld", "c.lw", "c.ldsp", "c.lwsp", "c.lbu", "c.lh", "c.lhu",
    "c.fld", "c.fldsp", "c.flw", "c.flwsp",
}


def functions(path, objdump):
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", path],
                         check=True, capture_output=True, text=True).stdout
    fns = {}
    cur = None
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:\s*$", line)
        if m:
            cur = m.group(1)
            fns[cur] = []
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s+(\S+)", line)
        if m and cur is not None:
            fns[cur].append((int(m.group(1), 16), m.group(2)))
    return fns


def marker_pc(fns, sym):
    body = fns.get(sym)
    if not body:
        raise SystemExit(f"symbol {sym!r} not found")
    for pc, mn in body:
        if mn in LOADS:
            return pc
    raise SystemExit(f"no load in {sym!r}")


def qemu_seq(path):
    seq = []
    for line in open(path, errors="replace"):
        m = re.search(r"\[[0-9a-f]+/([0-9a-f]+)/", line)
        if m:
            seq.append(int(m.group(1), 16))
    return seq


def gem5_seq(path):
    seq = []
    for line in open(path, errors="replace"):
        m = re.search(r"alloc sn=\d+ pc=(0x[0-9a-f]+)", line)
        if m:
            seq.append(int(m.group(1), 16))
    return seq


def distance(seq, loads, sensitive, marker):
    """Returns (base_pc, d) for the last marker, or None."""
    # load-only view
    lds = [p for p in seq if p in loads]
    idxs = [i for i, p in enumerate(lds) if p == marker]
    if not idxs:
        return None
    i = idxs[-1]
    for j in range(i - 1, -1, -1):
        if lds[j] in sensitive:
            return lds[j], (i - j - 1) + 1
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--sensitive", required=True,
                    help="comma-separated sensitive load PCs (hex)")
    ap.add_argument("--marker-sym", default="atk_load")
    ap.add_argument("--qemu-log")
    ap.add_argument("--gem5-log")
    ap.add_argument("--objdump", default="riscv64-linux-gnu-objdump")
    args = ap.parse_args()

    sensitive = {int(x, 16) for x in args.sensitive.split(",") if x.strip()}
    fns = functions(args.binary, args.objdump)
    mk = marker_pc(fns, args.marker_sym)
    loads = {pc for body in fns.values() for pc, mn in body if mn in LOADS}
    print(f"marker {args.marker_sym} = {mk:#x}; sensitive = "
          f"{', '.join(hex(p) for p in sorted(sensitive))}")

    for kind, path in (("architectural(qemu)", args.qemu_log),
                       ("dispatch(gem5)", args.gem5_log)):
        if not path:
            continue
        seq = qemu_seq(path) if kind.startswith("architectural") else gem5_seq(path)
        r = distance(seq, loads, sensitive, mk)
        if r is None:
            print(f"{kind:<20} no measurement")
        else:
            base, d = r
            print(f"{kind:<20} base={base:#x} d={d}")


if __name__ == "__main__":
    main()
