#!/usr/bin/env python3
"""Check the array_pattern microbenchmark trace (v1 or v0)."""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.dom_stage_a.analyzer import PerWordOracle
from tools.dom_stage_a.regions import RegionOracle
from tools.dom_stage_a.trace_replay import parse, replay


def parse_ranges(stdout_path):
    text = open(stdout_path, encoding="utf-8", errors="replace").read()
    m = re.search(r"affine=(0x[0-9a-fA-F]+) zeros=(0x[0-9a-fA-F]+) N=(\d+)", text)
    if not m:
        raise SystemExit("cannot find the 'affine=... zeros=... N=...' line")
    return int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))


def region_stats(res, lo, hi):
    selected = [(l, h) for l, h in res.results if lo <= l.addr < hi]
    total = len(selected)
    hits = sum(1 for _, h in selected if h)
    weight = sum(l.cost for l, _ in selected)
    wh = sum(l.cost for l, h in selected if h)
    return total, hits, weight, wh


def report(name, res, a_base, z_base, n):
    print(f"{name}: loads={res.loads} hits={res.hits} "
          f"count={res.count_fraction:.3f} "
          f"weighted={res.weight_fraction:.3f} "
          f"committed={res.committed} squashed={res.squashed} "
          f"unresolved={res.unresolved}")
    for rname, lo in (("affine", a_base), ("zeros", z_base)):
        t, h, w, wh = region_stats(res, lo, lo + 4 * n)
        print(f"  {rname:6s}: loads={t:7d} hits={h:7d} "
              f"count={h / t if t else 0:.3f} "
              f"weighted={wh / w if w else 0:.3f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--stdout", required=True)
    args = ap.parse_args()

    a_base, z_base, n = parse_ranges(args.stdout)
    loads, stores = parse(args.trace)

    pw = replay(loads, stores, PerWordOracle())
    rg = replay(loads, stores, RegionOracle(block_size=4096))

    report("per-word", pw, a_base, z_base, n)
    report("region4k", rg, a_base, z_base, n)

    ok = True

    def check(label, res, rname, lo, minimum):
        nonlocal ok
        t, h, _, _ = region_stats(res, lo, lo + 4 * n)
        frac = h / t if t else 0.0
        if t == 0 or frac < minimum:
            print(f"FAIL: {label} {rname} coverage {frac:.3f} < {minimum}",
                  file=sys.stderr)
            ok = False

    check("per-word", pw, "affine", a_base, 0.80)
    check("per-word", pw, "zeros", z_base, 0.30)
    check("region4k", rg, "affine", a_base, 0.80)
    check("region4k", rg, "zeros", z_base, 0.80)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
