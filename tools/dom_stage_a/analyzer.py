"""Stage A analyzer: pure Python, no gem5 dependency.

This module is the *offline* half of the Stage A profile.  It never touches a
trace produced by gem5 until A2; the tests in tests/dom_stage_a define the
contract, and the implementation below follows them.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass
from typing import Callable, Iterable, Optional, Sequence


class PatternKind(enum.Enum):
    ZERO = "zero"
    CONSTANT = "constant"
    AFFINE_INDEX = "affine_index"
    AFFINE_VALUE = "affine_value"
    POINTER_AFFINE = "pointer_affine"
    RUN = "run"
    NONE = "none"


@dataclass(frozen=True)
class Pattern:
    """A region descriptor: ``length`` words starting at ``base``.

    ``value_at`` must reconstruct the exact value for every word in the
    region, or return ``None`` outside it.  ``width`` is the word size in
    bytes (4 for RV32/64 words in this project).
    """

    kind: PatternKind
    base: int
    length: int
    p0: int = 0
    p1: int = 0
    width: int = 4

    def value_at(self, addr: int) -> Optional[int]:
        if self.width <= 0 or addr < self.base:
            return None
        idx, rem = divmod(addr - self.base, self.width)
        if rem != 0 or idx >= self.length:
            return None
        if self.kind is PatternKind.ZERO:
            return 0
        if self.kind is PatternKind.CONSTANT:
            return self.p0
        return self.p0 + idx * self.p1


@dataclass(frozen=True)
class Load:
    """One load in the delayed set, with its Stage A cost weight."""

    addr: int
    value: int
    tick: int
    cost: int
    seq: int = 0


@dataclass(frozen=True)
class Coverage:
    count_hits: int
    count_total: int
    weight_hits: int
    weight_total: int

    @property
    def count_fraction(self) -> float:
        return 0.0 if self.count_total == 0 else self.count_hits / self.count_total

    @property
    def weight_fraction(self) -> float:
        return 0.0 if self.weight_total == 0 else self.weight_hits / self.weight_total


def dom_cost(issue: int, safe: int, miss_latency: int) -> int:
    """Added completion time under DoM for one load.

    Baseline completion = max(safe, issue + L); DoM completion = safe + L.
    Added = min(L, max(0, safe - issue)).
    """
    latency = max(0, int(miss_latency))
    slack = max(0, int(safe) - int(issue))
    return min(latency, slack)


def detect_pattern(
    addrs: Sequence[int],
    values: Sequence[int],
    width: int = 4,
) -> Optional[Pattern]:
    """Recognise an exact pattern from ordered committed samples.

    Samples are matched in address order.  Returns ``None`` when no exact
    reconstruction is possible.
    """
    if width <= 0 or len(addrs) != len(values) or not addrs:
        return None

    pairs = sorted(zip(addrs, values), key=lambda p: p[0])
    base = pairs[0][0]
    # The descriptor reconstructs addresses as base + idx*width.  Refuse a
    # sample set that is not laid out that way, rather than mis-describe it.
    for idx, (addr, _) in enumerate(pairs):
        if addr != base + idx * width:
            return None

    vals = [v for _, v in pairs]
    length = len(vals)

    if all(v == 0 for v in vals):
        return Pattern(PatternKind.ZERO, base, length, 0, 0, width)

    if all(v == vals[0] for v in vals):
        return Pattern(PatternKind.CONSTANT, base, length, vals[0], 0, width)

    delta = vals[1] - vals[0]
    if delta == 0:
        return None
    for left, right in zip(vals, vals[1:]):
        if right - left != delta:
            return None

    if vals[0] == 0 and delta == 1:
        kind = PatternKind.AFFINE_INDEX
    elif delta % 64 == 0 and min(vals) >= 0x10000:
        # Heuristic: a large, cache-line-strided progression is an array of
        # pointers.  The distinction is semantic; the reconstruction is the
        # same affine function and is exact either way.
        kind = PatternKind.POINTER_AFFINE
    else:
        kind = PatternKind.AFFINE_VALUE

    return Pattern(kind, base, length, vals[0], delta, width)


class PerWordOracle:
    """Infinite per-word committed-value table (the T2 ceiling).

    Populated by committed stores and (by default) committed loads only.
    A lookup is causally valid only for events strictly after the last
    update tick.
    """

    def __init__(self, committed_only: bool = True) -> None:
        self.committed_only = committed_only
        self._values: dict[int, tuple[int, int]] = {}

    def _record(self, addr: int, size: int, value: int, tick: int) -> None:
        # Word-granular table, little-endian word extraction.
        for off in range(0, max(4, size), 4):
            if off >= size:
                break
            word = (value >> (8 * off)) & 0xFFFFFFFF
            self._values[addr + off] = (tick, word)

    def store(self, addr: int, size: int, value: int, tick: int) -> None:
        self._record(addr, size, value, tick)

    def load(self, addr: int, size: int, value: int, tick: int, committed: bool) -> None:
        if self.committed_only and not committed:
            return
        self._record(addr, size, value, tick)

    def lookup(self, addr: int, tick: int) -> Optional[int]:
        entry = self._values.get(addr)
        if entry is None or entry[0] >= tick:
            return None
        return entry[1]


def coverage(
    loads: Iterable[Load],
    lookup: Callable[[int, int], Optional[int]],
) -> Coverage:
    """Count- and cost-weighted coverage.  A hit must match the load value."""
    count_hits = count_total = weight_hits = weight_total = 0
    for load in loads:
        count_total += 1
        weight_total += load.cost
        value = lookup(load.addr, load.tick)
        if value is not None and value == load.value:
            count_hits += 1
            weight_hits += load.cost
    return Coverage(count_hits, count_total, weight_hits, weight_total)
