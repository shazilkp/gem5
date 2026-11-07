#!/usr/bin/env python3
"""Check provenance microbenchmark traces (spec-hit / squash)."""

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
    ap.add_argument("--mode", choices=["spec", "squash"], required=True)
    args = ap.parse_args()

    loads, stores = parse(args.trace)
    res = replay(loads, stores, PerWordOracle())

    spec = res.kind_loads.get(1, 0)
    spec_hits = res.kind_hits.get(1, 0)
    print(f"mode={args.mode} loads={res.loads} hits={res.hits} "
          f"committed={res.committed} squashed={res.squashed} "
          f"unresolved={res.unresolved} spec={spec} spec_hits={spec_hits} "
          f"weight={res.weight}")

    ok = True
    if args.mode == "spec":
        if spec == 0:
            print("FAIL: no spec-hit (L kind=1) in the trace", file=sys.stderr)
            ok = False
        if res.committed == 0:
            print("FAIL: no committed delayed load", file=sys.stderr)
            ok = False
    else:
        if res.squashed == 0:
            print("FAIL: no squashed load (X) in the trace", file=sys.stderr)
            ok = False
        if any(l.squashed for l, _ in res.results):
            print("FAIL: squashed load included in coverage", file=sys.stderr)
            ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
