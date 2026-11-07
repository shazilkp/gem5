"""Stage A analyzer contract.  These tests are written before the code."""

import unittest

from tools.dom_stage_a.analyzer import (
    Coverage,
    Load,
    Pattern,
    PatternKind,
    PerWordOracle,
    coverage,
    detect_pattern,
    dom_cost,
)


class TestCostFormula(unittest.TestCase):
    """cost = min(miss_latency, max(0, safe - issue))."""

    def test_slack_smaller_than_latency(self):
        self.assertEqual(dom_cost(0, 10, 100), 10)

    def test_slack_larger_than_latency(self):
        self.assertEqual(dom_cost(0, 200, 100), 100)

    def test_safe_at_issue(self):
        self.assertEqual(dom_cost(0, 0, 100), 0)

    def test_hit_has_zero_cost(self):
        self.assertEqual(dom_cost(0, 10, 0), 0)

    def test_safe_before_issue_is_clamped(self):
        self.assertEqual(dom_cost(10, 0, 100), 0)


class TestPatternDetection(unittest.TestCase):
    BASE = 0x1000

    def addrs(self, n):
        return [self.BASE + 4 * i for i in range(n)]

    def test_zero(self):
        p = detect_pattern(self.addrs(3), [0, 0, 0])
        self.assertIsNotNone(p)
        self.assertEqual(p.kind, PatternKind.ZERO)
        self.assertEqual([p.value_at(a) for a in self.addrs(3)], [0, 0, 0])

    def test_constant(self):
        p = detect_pattern(self.addrs(3), [7, 7, 7])
        self.assertIsNotNone(p)
        self.assertEqual(p.kind, PatternKind.CONSTANT)
        self.assertEqual([p.value_at(a) for a in self.addrs(3)], [7, 7, 7])

    def test_affine_value(self):
        p = detect_pattern(self.addrs(3), [100, 110, 120])
        self.assertIsNotNone(p)
        self.assertEqual(p.kind, PatternKind.AFFINE_VALUE)
        self.assertEqual([p.value_at(a) for a in self.addrs(3)], [100, 110, 120])

    def test_affine_index(self):
        p = detect_pattern(self.addrs(3), [0, 1, 2])
        self.assertIsNotNone(p)
        self.assertEqual(p.kind, PatternKind.AFFINE_INDEX)
        self.assertEqual([p.value_at(a) for a in self.addrs(3)], [0, 1, 2])

    def test_pointer_affine(self):
        vals = [0x100000, 0x100040, 0x100080]
        p = detect_pattern(self.addrs(3), vals)
        self.assertIsNotNone(p)
        self.assertEqual(p.kind, PatternKind.POINTER_AFFINE)
        self.assertEqual([p.value_at(a) for a in self.addrs(3)], vals)

    def test_unrecognised(self):
        self.assertIsNone(detect_pattern(self.addrs(3), [1, 9, 41]))

    def test_outside_region_is_none(self):
        p = detect_pattern(self.addrs(3), [0, 0, 0])
        self.assertIsNone(p.value_at(self.BASE + 400))
        self.assertIsNone(p.value_at(self.BASE - 4))


class TestPerWordOracle(unittest.TestCase):
    ADDR = 0x2000

    def test_store_then_lookup(self):
        o = PerWordOracle()
        o.store(self.ADDR, 4, 42, tick=10)
        self.assertEqual(o.lookup(self.ADDR, tick=11), 42)

    def test_lookup_is_causal(self):
        o = PerWordOracle()
        o.store(self.ADDR, 4, 42, tick=10)
        self.assertIsNone(o.lookup(self.ADDR, tick=9))

    def test_later_store_wins(self):
        o = PerWordOracle()
        o.store(self.ADDR, 4, 42, tick=10)
        o.store(self.ADDR, 4, 99, tick=20)
        self.assertEqual(o.lookup(self.ADDR, tick=21), 99)

    def test_squashed_load_does_not_populate(self):
        o = PerWordOracle()
        o.load(self.ADDR, 4, 7, tick=10, committed=False)
        self.assertIsNone(o.lookup(self.ADDR, tick=11))

    def test_committed_load_populates(self):
        o = PerWordOracle()
        o.load(self.ADDR, 4, 7, tick=10, committed=True)
        self.assertEqual(o.lookup(self.ADDR, tick=11), 7)

    def test_speculative_population_opt_in(self):
        o = PerWordOracle(committed_only=False)
        o.load(self.ADDR, 4, 7, tick=10, committed=False)
        self.assertEqual(o.lookup(self.ADDR, tick=11), 7)


class TestFiniteValueTable(unittest.TestCase):
    def test_lru_eviction(self):
        o = PerWordOracle(capacity=2)
        o.store(0x1000, 4, 1, tick=1)
        o.store(0x2000, 4, 2, tick=2)
        self.assertEqual(o.lookup(0x1000, tick=3), 1)  # touch 0x1000
        o.store(0x3000, 4, 3, tick=4)                  # evict LRU 0x2000
        self.assertIsNone(o.lookup(0x2000, tick=5))
        self.assertEqual(o.lookup(0x1000, tick=5), 1)
        self.assertEqual(o.lookup(0x3000, tick=5), 3)

    def test_infinite_by_default(self):
        o = PerWordOracle()
        for i in range(10):
            o.store(0x1000 + 4 * i, 4, i, tick=i + 1)
        self.assertEqual(o.lookup(0x1000, tick=100), 0)


class TestCoverage(unittest.TestCase):
    def test_count_and_weight(self):
        loads = [
            Load(addr=0x1000, value=42, tick=11, cost=100),
            Load(addr=0x2000, value=7, tick=11, cost=1),
        ]

        def lookup(addr, tick):
            return 42 if addr == 0x1000 else None

        cov = coverage(loads, lookup)
        self.assertEqual(cov.count_hits, 1)
        self.assertEqual(cov.count_total, 2)
        self.assertEqual(cov.weight_hits, 100)
        self.assertEqual(cov.weight_total, 101)
        self.assertAlmostEqual(cov.count_fraction, 0.5)
        self.assertAlmostEqual(cov.weight_fraction, 100 / 101)

    def test_wrong_value_is_not_a_hit(self):
        loads = [Load(addr=0x1000, value=42, tick=11, cost=100)]
        cov = coverage(loads, lambda addr, tick: 41)
        self.assertEqual(cov.count_hits, 0)
        self.assertEqual(cov.weight_hits, 0)


if __name__ == "__main__":
    unittest.main()
