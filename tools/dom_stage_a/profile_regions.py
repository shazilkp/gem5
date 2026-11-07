#!/usr/bin/env python3
"""Replay a Stage A trace through the region/compression oracle."""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.dom_stage_a.regions import RegionOracle
from tools.dom_stage_a.trace_replay import parse, replay


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--block-size", type=int, default=4096)
    ap.add_argument("--capacity", type=int, default=0,
                    help="descriptor budget; 0 means unlimited")
    ap.add_argument("--min-samples", type=int, default=2)
    args = ap.parse_args()

    capacity = None if args.capacity <= 0 else args.capacity
    oracle = RegionOracle(block_size=args.block_size, capacity=capacity,
                          min_samples=args.min_samples)

    loads, stores = parse(args.trace)
    res = replay(loads, stores, oracle)

    print(f"block={args.block_size} capacity={args.capacity} "
          f"min_samples={args.min_samples} stores={len(stores)} "
          f"loads={res.loads} hits={res.hits} "
          f"count={res.count_fraction:.4f} "
          f"weighted={res.weight_fraction:.4f} "
          f"committed={res.committed} squashed={res.squashed} "
          f"unresolved={res.unresolved} "
          f"descriptors={oracle.descriptors}")


if __name__ == "__main__":
    main()
