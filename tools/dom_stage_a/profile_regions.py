#!/usr/bin/env python3
"""Replay a Stage A v0 trace through the region/compression oracle.

Same caveat as profile_trace.py: v0 has no commit/squash information, so a
completed miss is treated as committed.  This is the T3 ceiling, not the
Stage A result.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.dom_stage_a.regions import RegionOracle


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--block-size", type=int, default=4096)
    ap.add_argument("--capacity", type=int, default=0,
                    help="descriptor budget; 0 means unlimited")
    ap.add_argument("--min-samples", type=int, default=2)
    ap.add_argument("--max-events", type=int, default=0)
    args = ap.parse_args()

    capacity = None if args.capacity <= 0 else args.capacity
    oracle = RegionOracle(block_size=args.block_size, capacity=capacity,
                          min_samples=args.min_samples)

    stores = loads = hits = weight = weight_hits = 0
    with open(args.trace, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            if args.max_events and lineno > args.max_events:
                break
            p = line.split()
            if not p:
                continue
            tick = int(p[1])
            if p[0] == "S":
                oracle.note_store(int(p[3], 16), int(p[4]), int(p[5], 16),
                                  tick)
                stores += 1
            elif p[0] == "M":
                addr = int(p[3], 16)
                size = int(p[4])
                value = int(p[5], 16)
                latency = int(p[6])
                loads += 1
                weight += latency
                if oracle.lookup(addr, tick) == value:
                    hits += 1
                    weight_hits += latency
                oracle.note_load(addr, size, value, tick, committed=True)

    print(f"block={args.block_size} capacity={args.capacity} "
          f"min_samples={args.min_samples} stores={stores} loads={loads} "
          f"hits={hits} count={hits / loads if loads else 0:.4f} "
          f"weighted={weight_hits / weight if weight else 0:.4f} "
          f"descriptors={oracle.descriptors}")


if __name__ == "__main__":
    main()
