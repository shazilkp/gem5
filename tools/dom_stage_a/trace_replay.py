"""Parse and replay Stage A traces (v0 M/S and v1 L/V/C/X/S).

The replay is causal: stores and committed loads populate the provider at
their own tick; a delayed load queries at its fill tick; the load's own
value only enters the table when it commits.  Squashed loads are excluded
from coverage and never populate the table.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple

from tools.dom_stage_a.analyzer import dom_cost


@dataclass
class LoadRec:
    seq: int
    tid: int
    pc: int
    addr: int
    size: int
    value: int
    issue: int
    latency: int
    kind: int
    fill_seq: int = 0
    commit_seq: int = 0
    fill_lat: int = 0
    v0: bool = False
    safe: Optional[int] = None
    commit_tick: Optional[int] = None
    squashed: bool = False

    @property
    def fill_tick(self) -> int:
        return self.issue if self.v0 else self.issue + self.latency

    @property
    def cost(self) -> int:
        if self.v0:
            return self.latency
        safe = self.safe if self.safe is not None else self.issue
        return dom_cost(self.issue, safe, self.latency)

    @property
    def committed(self) -> bool:
        return self.commit_tick is not None and not self.squashed


@dataclass
class ReplayResult:
    loads: int
    hits: int
    weight: int
    weight_hits: int
    committed: int
    squashed: int
    unresolved: int
    kind_loads: Dict[int, int] = field(default_factory=dict)
    kind_hits: Dict[int, int] = field(default_factory=dict)
    results: List[Tuple[LoadRec, bool]] = field(default_factory=list)

    @property
    def count_fraction(self) -> float:
        return 0.0 if self.loads == 0 else self.hits / self.loads

    @property
    def weight_fraction(self) -> float:
        return 0.0 if self.weight == 0 else self.weight_hits / self.weight


def parse(path: str) -> Tuple[Dict[int, LoadRec], List[Tuple[int, int, int, int]]]:
    loads: Dict[int, LoadRec] = {}
    stores: List[Tuple[int, int, int, int]] = []
    safe: Dict[int, int] = {}
    commits: Dict[int, int] = {}
    squashes = set()
    v0_seq = 0

    with open(path, encoding="utf-8") as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            tag = p[0]
            if tag == "L":
                seq = int(p[1])
                loads[seq] = LoadRec(
                    seq=seq,
                    tid=int(p[2]),
                    pc=int(p[3], 16),
                    addr=int(p[4], 16),
                    size=int(p[5]),
                    value=int(p[6], 16),
                    issue=int(p[7]),
                    latency=int(p[8]),
                    kind=int(p[9]),
                    fill_seq=int(p[10], 16),
                    commit_seq=int(p[11], 16),
                    fill_lat=int(p[12]),
                )
            elif tag == "V":
                safe[int(p[1])] = int(p[2])
            elif tag == "C":
                commits[int(p[1])] = int(p[2])
            elif tag == "X":
                squashes.add(int(p[1]))
            elif tag == "S":
                stores.append((int(p[1]), int(p[3], 16), int(p[4]),
                               int(p[5], 16)))
            elif tag == "M":
                v0_seq -= 1
                tick = int(p[1])
                loads[v0_seq] = LoadRec(
                    seq=v0_seq,
                    tid=0,
                    pc=int(p[2], 16),
                    addr=int(p[3], 16),
                    size=int(p[4]),
                    value=int(p[5], 16),
                    issue=tick,
                    latency=int(p[6]),
                    kind=0,
                    fill_lat=int(p[6]),
                    v0=True,
                    safe=tick,
                    commit_tick=tick,
                )

    for seq, load in loads.items():
        if load.v0:
            continue
        load.safe = safe.get(seq)
        load.commit_tick = commits.get(seq)
        load.squashed = seq in squashes

    return loads, stores


def _note_store(provider, addr, size, value, tick):
    if hasattr(provider, "note_store"):
        provider.note_store(addr, size, value, tick)
    else:
        provider.store(addr, size, value, tick)


def _note_load(provider, addr, size, value, tick, committed):
    if hasattr(provider, "note_load"):
        provider.note_load(addr, size, value, tick, committed=committed)
    else:
        provider.load(addr, size, value, tick, committed=committed)


def replay(loads: Dict[int, LoadRec],
           stores: List[Tuple[int, int, int, int]],
           provider) -> ReplayResult:
    # (tick, order, tag, payload); stores/commits before queries at the same tick.
    events = []
    for tick, addr, size, value in stores:
        events.append((tick, 0, "S", (addr, size, value)))
    for load in loads.values():
        if load.committed:
            # A v0 M is logged at its fill and treated as committed at the
            # same tick: populate after the query so it does not serve
            # itself.  A v1 load commits strictly after its fill.
            order = 3 if load.v0 else 1
            events.append((load.commit_tick, order, "C", load))
        events.append((load.fill_tick, 2, "L", load))
    events.sort(key=lambda e: (e[0], e[1]))

    hits = total = weight = weight_hits = 0
    kind_loads: Dict[int, int] = {}
    kind_hits: Dict[int, int] = {}
    results: List[Tuple[LoadRec, bool]] = []

    for tick, _, tag, payload in events:
        if tag == "S":
            addr, size, value = payload
            _note_store(provider, addr, size, value, tick)
        elif tag == "C":
            load = payload
            _note_load(provider, load.addr, load.size, load.value, tick, True)
        else:
            load = payload
            if not load.committed:
                continue
            total += 1
            weight += load.cost
            kind_loads[load.kind] = kind_loads.get(load.kind, 0) + 1
            hit = provider.lookup(load.addr, tick) == load.value
            results.append((load, hit))
            if hit:
                hits += 1
                weight_hits += load.cost
                kind_hits[load.kind] = kind_hits.get(load.kind, 0) + 1

    committed = sum(1 for l in loads.values() if l.committed)
    squashed = sum(1 for l in loads.values() if l.squashed)
    unresolved = len(loads) - committed - squashed

    return ReplayResult(total, hits, weight, weight_hits, committed, squashed,
                        unresolved, kind_loads, kind_hits, results)
