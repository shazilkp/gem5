#!/usr/bin/env python3
"""B5: a DoM+provider run must match the baseline and record provider hits."""

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("dom")
    args = ap.parse_args()

    base_out = open(os.path.join(args.base, "out.txt"), "rb").read()
    dom_out = open(os.path.join(args.dom, "out.txt"), "rb").read()
    hits = counter(args.dom, ".domProvHits")
    misses = counter(args.dom, ".domProvMisses")

    print(f"equal_output={base_out == dom_out} "
          f"prov_hits={hits:.0f} prov_misses={misses:.0f}")
    print(f"base_out={base_out!r} dom_out={dom_out!r}")

    ok = True
    if base_out != dom_out:
        print("FAIL: architectural output differs", file=sys.stderr)
        ok = False
    if hits <= 0:
        print("FAIL: the provider never served a shadowed L1 miss",
              file=sys.stderr)
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
