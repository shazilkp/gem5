#!/usr/bin/env python3
"""Smoke-check a Stage A trace (v0: load misses + committed stores)."""

import argparse
import os
import re
import sys

# Allow running this script directly from anywhere: put the repo root on
# sys.path so `tools.dom_stage_a` resolves.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.dom_stage_a.analyzer import PerWordOracle


def parse_ranges(stdout_path):
    text = open(stdout_path, encoding="utf-8", errors="replace").read()
    m = re.search(r"affine=(0x[0-9a-fA-F]+) zeros=(0x[0-9a-fA-F]+) N=(\d+)", text)
    if not m:
        raise SystemExit("cannot find the 'affine=... zeros=... N=...' line")
    return int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))


def parse_trace(path):
    events = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == "M":
                events.append(("M", int(p[1]), int(p[2], 16), int(p[3], 16),
                               int(p[4]), int(p[5], 16), int(p[6])))
            elif p[0] == "S":
                events.append(("S", int(p[1]), int(p[2], 16), int(p[3], 16),
                               int(p[4]), int(p[5], 16), 0))
    events.sort(key=lambda e: (e[1], 0 if e[0] == "S" else 1))
    return events


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--stdout", required=True)
    args = ap.parse_args()

    a_base, z_base, n = parse_ranges(args.stdout)
    events = parse_trace(args.trace)
    oracle = PerWordOracle()

    # hits, total, weighted hits, weighted total
    stats = {"affine": [0, 0, 0, 0], "zeros": [0, 0, 0, 0], "other": [0, 0, 0, 0]}

    def region(addr):
        if a_base <= addr < a_base + 4 * n:
            return "affine"
        if z_base <= addr < z_base + 4 * n:
            return "zeros"
        return "other"

    for e in events:
        if e[0] == "S":
            _, tick, pc, addr, size, value, _ = e
            oracle.store(addr, size, value, tick)
        else:
            _, tick, pc, addr, size, value, latency = e
            s = stats[region(addr)]
            s[1] += 1
            s[3] += latency
            if oracle.lookup(addr, tick) == value:
                s[0] += 1
                s[2] += latency
            # v0 has no commit/squash info; treat a completed miss as committed.
            oracle.load(addr, size, value, tick, committed=True)

    total = sum(s[1] for s in stats.values())
    hits = sum(s[0] for s in stats.values())
    print(f"events={len(events)} loads={total} hits={hits} "
          f"count={hits / total if total else 0:.3f}")
    for name, s in stats.items():
        if s[1]:
            print(f"  {name:6s}: loads={s[1]:7d} hits={s[0]:7d} "
                  f"count={s[0] / s[1]:.3f} "
                  f"weighted={s[2] / s[3] if s[3] else 0:.3f}")

    ok = True
    if stats["affine"][1] == 0:
        print("FAIL: no affine-region loads in the trace", file=sys.stderr)
        ok = False
    elif stats["affine"][0] / stats["affine"][1] < 0.80:
        print("FAIL: affine coverage below 0.80", file=sys.stderr)
        ok = False
    if stats["zeros"][1] == 0:
        print("FAIL: no zeros-region loads in the trace", file=sys.stderr)
        ok = False
    elif stats["zeros"][0] / stats["zeros"][1] < 0.30:
        print("FAIL: zeros coverage below 0.30", file=sys.stderr)
        ok = False
    if total == 0 or hits / total < 0.50:
        print("FAIL: overall coverage below 0.50", file=sys.stderr)
        ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
