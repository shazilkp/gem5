#!/usr/bin/env python3
"""Replay a Stage A trace through the per-word committed-value table.

Supports v1 (L/V/C/X/S) and v0 (M/S) traces.  v0 treats a completed miss
as committed and weights by latency, matching the earlier runs.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.dom_stage_a.analyzer import PerWordOracle
from tools.dom_stage_a.trace_replay import parse, replay


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--capacity", type=int, default=0,
                    help="word entries; 0 means unlimited")
    args = ap.parse_args()

    capacity = None if args.capacity <= 0 else args.capacity
    oracle = PerWordOracle(capacity=capacity)

    loads, stores = parse(args.trace)
    res = replay(loads, stores, oracle)

    print(f"capacity={args.capacity} stores={len(stores)} loads={res.loads} "
          f"hits={res.hits} count={res.count_fraction:.4f} "
          f"weighted={res.weight_fraction:.4f} "
          f"committed={res.committed} squashed={res.squashed} "
          f"unresolved={res.unresolved} "
          f"kind0={res.kind_loads.get(0, 0)}/{res.kind_hits.get(0, 0)} "
          f"kind1={res.kind_loads.get(1, 0)}/{res.kind_hits.get(1, 0)} "
          f"entries={len(oracle._values)}")


if __name__ == "__main__":
    main()
