#!/usr/bin/env python3
"""B0/B1: baseline vs DoM must be architecturally equal, and the DoM run
must actually exercise the shadowed-miss path.

This test is RED until B1 exists: GEM5_DOM has no effect, so the two IPCs
are equal, and the DoM delay/release counters stay at zero.
"""

import argparse
import os
import sys


WANTED = {
    "system.cpu.commitStats0.ipc",
    "system.cpu.commitStats0.numInsts",
    "system.cpu.numCycles",
}


def read_stats(path):
    stats = {}
    dom_delayed = 0.0
    dom_released = 0.0
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue
            if parts[0] in WANTED:
                stats[parts[0]] = float(parts[1])
            if parts[0].endswith(".domDelayedMisses"):
                dom_delayed += float(parts[1])
            if parts[0].endswith(".domReleased"):
                dom_released += float(parts[1])
    stats["domDelayedMisses"] = dom_delayed
    stats["domReleased"] = dom_released
    return stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("dom")
    args = ap.parse_args()

    base_out = open(os.path.join(args.base, "out.txt"), "rb").read()
    dom_out = open(os.path.join(args.dom, "out.txt"), "rb").read()
    base = read_stats(os.path.join(args.base, "gem5", "stats.txt"))
    dom = read_stats(os.path.join(args.dom, "gem5", "stats.txt"))

    bi = base.get("system.cpu.commitStats0.ipc", 0.0)
    di = dom.get("system.cpu.commitStats0.ipc", 0.0)
    print(f"base_ipc={bi:.6f} dom_ipc={di:.6f} equal_output={base_out == dom_out}")
    print(f"base_out={base_out!r} dom_out={dom_out!r}")
    print(f"dom_delayed={dom['domDelayedMisses']:.0f} "
          f"dom_released={dom['domReleased']:.0f}")

    ok = True
    if base_out != dom_out:
        print("FAIL: architectural output differs", file=sys.stderr)
        ok = False
    if dom["domDelayedMisses"] <= 0:
        print("FAIL: DoM did not drop a shadowed L1 miss",
              file=sys.stderr)
        ok = False
    if dom["domReleased"] <= 0:
        print("FAIL: DoM did not re-issue a delayed load",
              file=sys.stderr)
        ok = False
    if not (di < bi):
        print("FAIL: DoM did not delay the shadowed miss "
              f"(dom IPC {di:.6f} >= base IPC {bi:.6f})", file=sys.stderr)
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
