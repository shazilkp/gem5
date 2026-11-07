#!/usr/bin/env python3
"""Replay a Stage A v0 trace and report committed-value coverage.

v0 has no commit/squash information, so a completed miss is treated as
committed.  The number is therefore an optimistic lower bound on the
delayed set and must not be reported as the Stage A result.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.dom_stage_a.analyzer import PerWordOracle


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--max-events", type=int, default=0)
    args = ap.parse_args()

    oracle = PerWordOracle()
    loads = hits = weight = weight_hits = stores = 0
    last_tick = -1
    out_of_order = 0

    with open(args.trace, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            if args.max_events and lineno > args.max_events:
                break
            p = line.split()
            if not p:
                continue
            tick = int(p[1])
            if tick < last_tick:
                out_of_order += 1
            last_tick = max(last_tick, tick)

            if p[0] == "S":
                addr = int(p[3], 16)
                size = int(p[4])
                value = int(p[5], 16)
                oracle.store(addr, size, value, tick)
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
                oracle.load(addr, size, value, tick, committed=True)

    print(f"stores={stores} loads={loads} hits={hits} "
          f"count={hits / loads if loads else 0:.4f} "
          f"weighted={weight_hits / weight if weight else 0:.4f} "
          f"entries={len(oracle._values)} tick_order_violations={out_of_order}")


if __name__ == "__main__":
    main()
