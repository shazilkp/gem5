#!/usr/bin/env python3
"""Stress/liveness checks for Delay-on-Miss.

- load_chain: a serial miss chain must complete (the runner's hard timeout
  catches self-shadow and missed-release hangs) and stay architecturally
  equal.  It must also exercise the delayed-miss path.
- hit_loop: an all-hit workload must not lose more than 10% IPC; this catches
  implementations that delay hits instead of only misses.
"""

import argparse
import os
import sys


def stats(run):
    out = {}
    with open(os.path.join(run, "gem5", "stats.txt"), encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                out[parts[0]] = parts[1]
    return out


def counter(run, suffix):
    return sum(float(v) for k, v in stats(run).items() if k.endswith(suffix))


def ipc(run):
    return float(stats(run).get("system.cpu.commitStats0.ipc", 0.0))


def check_pair(name, base_dir, dom_dir, min_ipc_ratio):
    base_out = open(os.path.join(base_dir, "out.txt"), "rb").read()
    dom_out = open(os.path.join(dom_dir, "out.txt"), "rb").read()
    bi = ipc(base_dir)
    di = ipc(dom_dir)
    delayed = counter(dom_dir, ".domDelayedMisses")
    ratio = (di / bi) if bi > 0 else 0.0

    print(f"{name}: equal={base_out == dom_out} base_ipc={bi:.6f} "
          f"dom_ipc={di:.6f} ratio={ratio:.3f} delayed={delayed:.0f}")

    ok = True
    if base_out != dom_out:
        print(f"FAIL {name}: architectural output differs", file=sys.stderr)
        ok = False
    if ratio < min_ipc_ratio:
        print(f"FAIL {name}: DoM IPC ratio {ratio:.3f} < "
              f"{min_ipc_ratio:.3f}", file=sys.stderr)
        ok = False
    return ok, delayed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", help="directory containing <prog>_base/<prog>_dom")
    args = ap.parse_args()

    ok = True

    chain_ok, chain_delayed = check_pair(
        "load_chain", os.path.join(args.out, "load_chain_base"),
        os.path.join(args.out, "load_chain_dom"), 0.0)
    ok &= chain_ok
    if chain_delayed <= 0:
        print("FAIL load_chain: no delayed miss was exercised", file=sys.stderr)
        ok = False

    hit_ok, _ = check_pair(
        "hit_loop", os.path.join(args.out, "hit_loop_base"),
        os.path.join(args.out, "hit_loop_dom"), 0.90)
    ok &= hit_ok

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
