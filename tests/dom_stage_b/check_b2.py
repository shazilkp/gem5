#!/usr/bin/env python3
"""B2: a shadowed L1 hit must defer its replacement update and apply it at
release.  The cache counters must be non-zero and the output must match."""

import argparse
import os
import sys


def read(run):
    stats = {}
    with open(os.path.join(run, "gem5", "stats.txt"), encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                stats[parts[0]] = parts[1]
    return stats


def counters(run):
    stats = read(run)
    deferred = 0.0
    released = 0.0
    for key, value in stats.items():
        if key.endswith(".domHitsDeferred"):
            deferred += float(value)
        if key.endswith(".domHitsReleased"):
            released += float(value)
    return deferred, released


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("dom")
    args = ap.parse_args()

    base_out = open(os.path.join(args.base, "out.txt"), "rb").read()
    dom_out = open(os.path.join(args.dom, "out.txt"), "rb").read()
    deferred, released = counters(args.dom)

    print(f"equal_output={base_out == dom_out} "
          f"deferred={deferred:.0f} released={released:.0f}")
    print(f"base_out={base_out!r} dom_out={dom_out!r}")

    ok = True
    if base_out != dom_out:
        print("FAIL: architectural output differs", file=sys.stderr)
        ok = False
    if deferred <= 0:
        print("FAIL: no shadowed hit had its replacement update deferred",
              file=sys.stderr)
        ok = False
    if released <= 0:
        print("FAIL: no deferred replacement update was applied at release",
              file=sys.stderr)
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
