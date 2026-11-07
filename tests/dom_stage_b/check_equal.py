#!/usr/bin/env python3
"""B1 architectural-equality regression across several microbenchmarks.

Each pair is (baseline_run, dom_run).  The program output must be byte-equal,
both runs must exit cleanly, and at least one DoM run must demonstrate that
the shadowed-miss drop/re-issue path was actually exercised.
"""

import os
import sys


def read(path):
    stats = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue
            stats[parts[0]] = parts[1]
    return stats


def dom_counters(run):
    stats = read(os.path.join(run, "gem5", "stats.txt"))
    delayed = 0.0
    released = 0.0
    for key, value in stats.items():
        if key.endswith(".domDelayedMisses"):
            delayed += float(value)
        if key.endswith(".domReleased"):
            released += float(value)
    return delayed, released


def main():
    args = sys.argv[1:]
    if not args or len(args) % 2 != 0:
        print("usage: check_equal.py base dom [base dom ...]", file=sys.stderr)
        return 2

    ok = True
    any_delayed = False
    any_released = False
    for base, dom in zip(args[0::2], args[1::2]):
        name = os.path.basename(base)
        base_out = open(os.path.join(base, "out.txt"), "rb").read()
        dom_out = open(os.path.join(dom, "out.txt"), "rb").read()
        delayed, released = dom_counters(dom)

        if base_out != dom_out:
            print(f"FAIL {name}: output differs\n  base={base_out!r}\n  dom={dom_out!r}",
                  file=sys.stderr)
            ok = False
        else:
            print(f"ok {name}: output equal; delayed={delayed:.0f} "
                  f"released={released:.0f}")

        any_delayed |= delayed > 0
        any_released |= released > 0

    if not any_delayed:
        print("FAIL: no run exercised a shadowed miss drop", file=sys.stderr)
        ok = False
    if not any_released:
        print("FAIL: no run re-issued a delayed load", file=sys.stderr)
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
