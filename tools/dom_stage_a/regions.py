"""Region/compression oracle for Stage A (T3 ceiling).

A block (page, line, or arbitrary aligned chunk) is described by one exact
pattern derived causally from committed observations.  A query predicts the
word from the pattern; the caller compares it with the real value, so a
descriptor that extrapolates incorrectly simply misses and is invalidated.
"""

from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass
from typing import Optional

from tools.dom_stage_a.analyzer import Pattern, PatternKind


@dataclass
class _Block:
    base: int
    count: int = 0
    first_idx: int = 0
    first_val: int = 0
    second_idx: Optional[int] = None
    second_val: int = 0
    delta: int = 0
    affine_ok: bool = False
    all_zero: bool = True
    all_equal: bool = True
    pattern: Optional[Pattern] = None
    last_used: int = 0
    in_table: bool = False


class RegionOracle:
    def __init__(self, block_size: int = 4096, capacity: Optional[int] = None,
                 width: int = 4, min_samples: int = 2):
        if block_size <= 0 or width <= 0:
            raise ValueError("block_size and width must be positive")
        self.block_size = block_size
        self.capacity = capacity
        self.width = width
        self.min_samples = max(1, min_samples)
        self._blocks: dict[int, _Block] = {}
        self._table: OrderedDict[int, _Block] = OrderedDict()

    # -- public ---------------------------------------------------------

    def note_store(self, addr: int, size: int, value: int, tick: int) -> None:
        self._observe(addr, value, tick)

    def note_load(self, addr: int, size: int, value: int, tick: int,
                  committed: bool = True) -> None:
        if not committed:
            return
        self._observe(addr, value, tick)

    def lookup(self, addr: int, tick: int) -> Optional[int]:
        block = self._blocks.get(addr & ~(self.block_size - 1))
        if block is None or block.pattern is None or not block.in_table:
            return None
        value = block.pattern.value_at(addr)
        if value is not None:
            block.last_used = tick
            self._table.move_to_end(block.base)
        return value

    @property
    def descriptors(self) -> int:
        return len(self._table)

    # -- internals ------------------------------------------------------

    def _observe(self, addr: int, value: int, tick: int) -> None:
        if addr % self.width:
            return
        base = addr & ~(self.block_size - 1)
        st = self._blocks.get(base)
        if st is None:
            st = _Block(base=base)
            self._blocks[base] = st

        idx = (addr - base) // self.width
        if st.count == 0:
            st.first_idx = idx
            st.first_val = value
            st.all_zero = (value == 0)
            st.all_equal = True
        else:
            st.all_zero = st.all_zero and value == 0
            st.all_equal = st.all_equal and value == st.first_val

            if st.second_idx is None:
                if idx != st.first_idx:
                    st.second_idx = idx
                    st.second_val = value
                    di = idx - st.first_idx
                    diff = value - st.first_val
                    if di != 0 and diff % di == 0:
                        st.delta = diff // di
                        st.affine_ok = True
                    else:
                        st.affine_ok = False
                else:
                    if value != st.first_val:
                        st.first_val = value
                        st.affine_ok = False
            elif st.affine_ok:
                if idx == st.first_idx:
                    if value != st.first_val:
                        st.affine_ok = False
                elif value != st.first_val + (idx - st.first_idx) * st.delta:
                    st.affine_ok = False

        st.count += 1
        st.last_used = tick
        st.pattern = self._refit(st)
        self._ensure_table(st, tick)

    def _refit(self, st: _Block) -> Optional[Pattern]:
        if st.count < self.min_samples:
            return None
        length = self.block_size // self.width
        if st.all_zero:
            return Pattern(PatternKind.ZERO, st.base, length, 0, 0, self.width)
        if st.all_equal:
            return Pattern(PatternKind.CONSTANT, st.base, length, st.first_val,
                           0, self.width)
        if st.affine_ok:
            p0 = st.first_val - st.first_idx * st.delta
            if p0 == 0 and st.delta == 1:
                kind = PatternKind.AFFINE_INDEX
            elif st.delta % 64 == 0 and st.first_val >= 0x10000:
                kind = PatternKind.POINTER_AFFINE
            else:
                kind = PatternKind.AFFINE_VALUE
            return Pattern(kind, st.base, length, p0, st.delta, self.width)
        return None

    def _ensure_table(self, st: _Block, tick: int) -> None:
        if st.pattern is None:
            self._remove(st)
            return

        if st.in_table:
            self._table.move_to_end(st.base)
        else:
            if self.capacity is not None and len(self._table) >= self.capacity:
                _, victim = self._table.popitem(last=False)
                victim.in_table = False
            st.in_table = True
            self._table[st.base] = st
        st.last_used = tick

    def _remove(self, st: _Block) -> None:
        if st.in_table:
            st.in_table = False
            self._table.pop(st.base, None)
