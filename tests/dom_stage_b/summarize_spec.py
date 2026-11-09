#!/usr/bin/env python3
"""Summarize the Stage B SPEC arm results.

Reads the per-benchmark ipc.tsv files written by run_spec_arms.sh and prints
the decisive comparisons:

  DoM cost            = 1 - IPC(dom) / IPC(base)
  provider recovery   = (IPC(prov4k) - IPC(dom)) / (IPC(base) - IPC(dom))
  equal-area L1 check = IPC(prov4k) vs IPC(dom_l1_128k)
  equal-area L2 check = IPC(prov4k) vs IPC(dom_l2_64k)

A positive recovery means the provider bought back some of the DoM cost.  If
the provider and the equal-area cache have essentially the same IPC, the
provider is behaving like a cache on that workload, which is a reportable
result.

The primary comparison uses the 4K provider (~64 KiB) against the 128 KiB L1
and 64 KiB L2.  The 65K provider and 1 MiB L1 arms are out-of-scope
sensitivity points and are deliberately not printed.
"""

import argparse
import csv
import glob
import os
import sys

ARMS = [
    "base",
    "dom",
    "dom_prov4k",
    "dom_prov65k",
    "dom_l1_128k",
    "dom_l1_1m",
    "base_l1_1m",
]

COLUMNS = [
    "bench",
    "arm",
    "ipc",
    "insts",
    "cycles",
    "dcacheMisses",
    "provHits",
    "domDelayed",
    "domHitDeferred",
]


def load_rows(paths):
    rows = {}
    for path in paths:
        with open(path, encoding="utf-8") as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) != len(COLUMNS):
                    continue
                row = dict(zip(COLUMNS, parts))
                if row["arm"] == "arm" or row["ipc"] == "ipc":
                    continue
                rows[(row["bench"], row["arm"])] = row
    return rows


def value(rows, bench, arm, key, default=0.0):
    row = rows.get((bench, arm))
    if row is None:
        return default
    try:
        return float(row[key])
    except (KeyError, ValueError):
        return default


def pct(x):
    return f"{100.0 * x:+.2f}%"


def recovery(prov, dom, base):
    denom = base - dom
    if denom <= 1e-9:
        return None
    return (prov - dom) / denom


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--glob", default="/tmp/opencode/dom_stage_b/spec/*/ipc.tsv")
    ap.add_argument("--csv", default=None)
    args = ap.parse_args()

    paths = sorted(glob.glob(args.glob))
    if not paths:
        print(f"no ipc.tsv files match {args.glob}", file=sys.stderr)
        return 1

    rows = load_rows(paths)
    benches = sorted({bench for bench, _ in rows})

    header = (
        "| bench | base | DoM | DoM cost | prov4k | recovery | "
        "L1 128k | L2 64k | prov vs L1 | prov vs L2 |"
    )
    print(header)
    print("|" + "---|" * 11)

    out_rows = []
    for bench in benches:
        base = value(rows, bench, "base", "ipc")
        dom = value(rows, bench, "dom", "ipc")
        p4 = value(rows, bench, "dom_prov4k", "ipc")
        l128 = value(rows, bench, "dom_l1_128k", "ipc")
        l2 = value(rows, bench, "dom_l2_64k", "ipc")
        b128 = value(rows, bench, "base_l1_128k", "ipc")
        b2 = value(rows, bench, "base_l2_64k", "ipc")

        cost = (1.0 - dom / base) if base > 0 else 0.0
        r4 = recovery(p4, dom, base)
        vs_l1 = (p4 - l128) / l128 if l128 > 0 else 0.0
        vs_l2 = (p4 - l2) / l2 if l2 > 0 else 0.0

        def fmt_recovery(r):
            return "n/a" if r is None else pct(r)

        print(
            f"| {bench} | {base:.4f} | {dom:.4f} | {pct(cost)} | "
            f"{p4:.4f} | {fmt_recovery(r4)} | {l128:.4f} | {l2:.4f} | "
            f"{pct(vs_l1)} | {pct(vs_l2)} |"
        )

        out_rows.append({
            "bench": bench,
            "base_ipc": base,
            "dom_ipc": dom,
            "dom_cost": cost,
            "prov4k_ipc": p4,
            "prov4k_recovery": r4 if r4 is not None else "",
            "l1_128k_ipc": l128,
            "l2_64k_ipc": l2,
            "base_l1_128k_ipc": b128,
            "base_l2_64k_ipc": b2,
            "prov4k_vs_l1_128k": vs_l1,
            "prov4k_vs_l2_64k": vs_l2,
            "provHits": value(rows, bench, "dom_prov4k", "provHits"),
            "domDelayed": value(rows, bench, "dom", "domDelayed"),
        })

    print("\nPrimary comparison: 4K provider (~64 KiB) vs 128 KiB L1 and "
          "64 KiB L2.\nThe 65K provider and 1 MiB L1 arms are out-of-scope "
          "sensitivity points and are deliberately not used here.")

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=list(out_rows[0].keys()))
            writer.writeheader()
            writer.writerows(out_rows)
        print(f"\nwrote {args.csv}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
