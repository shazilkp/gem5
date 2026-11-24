#!/usr/bin/env python3
"""Reconstruct the transient-window margin from an EgDiff debug trace.

The trace must be produced with:

    --debug-flags=EgDiff --debug-file=simout

The relevant lines (added by the race experiment) are:

    [tick:T] schedule PC 0x... [sn:N] predicted val 0x...
    [tick:T] publish PC 0x... sn:N val 0x...
    [tick:T] load-complete PC 0x... sn:N addr 0x...
    [tick:T] [sn:N] MISMATCH actual 0x...
    [tick:T] squash PC 0x... sn:N

For each MISMATCH (the marker load), the script finds the matching publish,
the marker's load-complete, the first probe-line completion after publish and
before the squash, and the squash.  It reports the margin in cycles:

    probe_complete - publish
    verify        - probe_complete
    squash        - probe_complete

Usage:
    measure_race_margin.py --log simout --probe-base 0x... --probe-size 16384
                           [--ticks-per-cycle 500]
"""

import argparse
import re
import sys

TICK = r"\[tick:(\d+)\]"
RE_SCHED = re.compile(TICK + r" schedule PC (0x[0-9a-fA-F]+) \[sn:(\d+)\]")
RE_PUB = re.compile(TICK + r" publish PC (0x[0-9a-fA-F]+) sn:(\d+)")
RE_COMPLETE = re.compile(
    TICK + r" load-complete PC (0x[0-9a-fA-F]+) sn:(\d+) addr (0x[0-9a-fA-F]+)")
RE_REQUEST = re.compile(
    TICK + r" load-request PC (0x[0-9a-fA-F]+) sn:(\d+) addr (0x[0-9a-fA-F]+)")
RE_SENT = re.compile(
    TICK + r" load-sent PC (0x[0-9a-fA-F]+) sn:(\d+) addr (0x[0-9a-fA-F]+)")
RE_MISMATCH = re.compile(TICK + r" \[sn:(\d+)\] MISMATCH")
RE_SQUASH = re.compile(TICK + r" squash PC (0x[0-9a-fA-F]+) sn:(\d+)")
# LVP deferred publish: the gem5 log prefix is the tick.
RE_DEF_PUB = re.compile(
    r"^\s*(\d+): .*deferred wakeup -> publish 0x[0-9a-fA-F]+ to "
    r"dependent PC 0x[0-9a-fA-F]+ \[dep:(\d+)\]")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--probe-base", required=True, type=lambda x: int(x, 0))
    ap.add_argument("--probe-size", type=lambda x: int(x, 0), default=16384)
    # This configuration is 2 GHz: 500 ticks/cycle.  Verify with
    # simTicks / system.cpu.numCycles if the clock changes.
    ap.add_argument("--ticks-per-cycle", type=int, default=500)
    args = ap.parse_args()

    probe_lo = args.probe_base
    probe_hi = args.probe_base + args.probe_size
    tp = args.ticks_per_cycle

    schedules = {}
    publishes = {}
    completes = {}
    requests = []
    sends = []
    mismatches = []
    squashes = []

    with open(args.log, errors="replace") as f:
        for line in f:
            m = RE_SCHED.search(line)
            if m:
                schedules[int(m.group(3))] = int(m.group(1))
                continue
            m = RE_DEF_PUB.search(line)
            if m:
                publishes[int(m.group(2))] = int(m.group(1))
                continue
            m = RE_PUB.search(line)
            if m:
                publishes[int(m.group(3))] = int(m.group(1))
                continue
            m = RE_SENT.search(line)
            if m:
                sends.append((int(m.group(1)), int(m.group(4), 16),
                              int(m.group(3))))
                continue
            m = RE_REQUEST.search(line)
            if m:
                requests.append((int(m.group(1)), int(m.group(4), 16),
                                 int(m.group(3))))
                continue
            m = RE_COMPLETE.search(line)
            if m:
                sn = int(m.group(3))
                completes.setdefault(sn, []).append(
                    (int(m.group(1)), int(m.group(4), 16),
                     int(m.group(2), 16)))
                continue
            m = RE_MISMATCH.search(line)
            if m:
                mismatches.append((int(m.group(1)), int(m.group(2))))
                continue
            m = RE_SQUASH.search(line)
            if m:
                squashes.append((int(m.group(1)), int(m.group(3))))

    if not mismatches:
        print("no MISMATCH lines found", file=sys.stderr)
        return 1

    print("sn\tpub\tprobe_req\tverify\tsquash\t"
          "pub->req\tverify->req\treq->squash")
    for verify, sn in mismatches:
        pub = publishes.get(sn)
        if pub is None:
            # Normal (non-deferred) predictions publish at dispatch.
            pub = schedules.get(sn)
        if pub is None:
            continue
        # First probe-array completion after publish, before the squash.
        sq = None
        for t, s in squashes:
            if s == sn:
                sq = t
                break
        # The probe is the load whose request address is in the probe array.
        # Use the request (cache access start), not completion: the fill can
        # land after the squash and still be visible.
        pt = None
        for t, addr, _osn in sends:
            if probe_lo <= addr < probe_hi and t >= pub:
                if sq is not None and t > sq:
                    continue
                if pt is None or t < pt:
                    pt = t
        if pt is None:
            print(f"{sn}\t{pub}\t-\t{verify}\t{sq}\t-\t-\t-")
            continue
        print(f"{sn}\t{pub}\t{pt}\t{verify}\t{sq}\t"
              f"{(pt-pub)/tp:.1f}\t{(verify-pt)/tp:.1f}\t"
              f"{((sq-pt)/tp if sq is not None else float('nan')):.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
