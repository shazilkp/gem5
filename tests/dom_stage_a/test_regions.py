"""Contract for the region/compression oracle."""

import unittest

from tools.dom_stage_a.regions import RegionOracle


class TestRegionOracle(unittest.TestCase):
    def test_min_samples_is_causal(self):
        o = RegionOracle(block_size=4096, min_samples=2)
        o.note_store(0x10000, 4, 0, tick=1)
        self.assertIsNone(o.lookup(0x10000, tick=2))
        o.note_store(0x10004, 4, 0, tick=3)
        self.assertEqual(o.lookup(0x10000, tick=4), 0)

    def test_zero_block_extrapolates(self):
        o = RegionOracle(block_size=4096)
        o.note_store(0x10000, 4, 0, tick=1)
        o.note_store(0x10004, 4, 0, tick=2)
        self.assertEqual(o.lookup(0x10800, tick=3), 0)

    def test_affine_block_extrapolates(self):
        o = RegionOracle(block_size=64)
        o.note_store(0x1000, 4, 100, tick=1)
        o.note_store(0x1004, 4, 103, tick=2)
        self.assertEqual(o.lookup(0x1028, tick=3), 130)

    def test_index_block(self):
        o = RegionOracle(block_size=64)
        o.note_store(0x2000, 4, 0, tick=1)
        o.note_store(0x2004, 4, 1, tick=2)
        self.assertEqual(o.lookup(0x2010, tick=3), 4)

    def test_deviating_store_invalidates(self):
        o = RegionOracle(block_size=64)
        o.note_store(0x1000, 4, 100, tick=1)
        o.note_store(0x1004, 4, 103, tick=2)
        self.assertEqual(o.lookup(0x1008, tick=3), 106)
        o.note_store(0x1008, 4, 999, tick=4)
        self.assertIsNone(o.lookup(0x100c, tick=5))

    def test_consistent_store_keeps_pattern(self):
        o = RegionOracle(block_size=64)
        o.note_store(0x1000, 4, 100, tick=1)
        o.note_store(0x1004, 4, 103, tick=2)
        o.note_store(0x1008, 4, 106, tick=3)
        self.assertEqual(o.lookup(0x100c, tick=4), 109)

    def test_committed_load_can_establish_pattern(self):
        o = RegionOracle(block_size=64)
        o.note_load(0x3000, 4, 0, tick=1, committed=True)
        o.note_load(0x3004, 4, 0, tick=2, committed=True)
        self.assertEqual(o.lookup(0x3008, tick=3), 0)

    def test_deviating_load_invalidates(self):
        o = RegionOracle(block_size=64)
        o.note_load(0x1000, 4, 100, tick=1, committed=True)
        o.note_load(0x1004, 4, 103, tick=2, committed=True)
        o.note_load(0x1008, 4, 999, tick=3, committed=True)
        self.assertIsNone(o.lookup(0x100c, tick=4))

    def test_block_boundary(self):
        o = RegionOracle(block_size=64)
        o.note_store(0x1000, 4, 100, tick=1)
        o.note_store(0x1004, 4, 103, tick=2)
        self.assertIsNone(o.lookup(0x1040, tick=3))  # next block, no samples

    def test_capacity_evicts_lru(self):
        o = RegionOracle(block_size=64, capacity=1)
        o.note_store(0x1000, 4, 1, tick=1)
        o.note_store(0x1004, 4, 1, tick=2)   # block A descriptor
        o.note_store(0x2000, 4, 2, tick=3)
        o.note_store(0x2004, 4, 2, tick=4)   # block B, evicts A
        self.assertIsNone(o.lookup(0x1008, tick=5))
        self.assertEqual(o.lookup(0x2008, tick=5), 2)

    def test_descriptor_count(self):
        o = RegionOracle(block_size=64)
        o.note_store(0x1000, 4, 1, tick=1)
        o.note_store(0x1004, 4, 1, tick=2)
        o.note_store(0x2000, 4, 2, tick=3)
        o.note_store(0x2004, 4, 2, tick=4)
        self.assertEqual(o.descriptors, 2)


if __name__ == "__main__":
    unittest.main()
