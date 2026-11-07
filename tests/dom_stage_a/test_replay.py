"""Contract for the v1 trace parser and causal replay."""

import os
import tempfile
import unittest

from tools.dom_stage_a.analyzer import PerWordOracle
from tools.dom_stage_a.trace_replay import parse, replay


V1_TRACE = """\
L 1 0 1000 10000 4 64 10 100 0 1 0 100
V 1 50
C 1 200
S 300 0 10000 4 64
L 2 0 1000 10000 4 64 310 100 0 2 200 100
V 2 350
C 2 400
L 5 0 1000 20000 4 77 500 100 0 3 0 100
X 5 600
"""

V0_TRACE = """\
M 10 1000 10000 4 64 100
S 20 0 10000 4 64
M 30 1000 10000 4 64 100
"""


def _write(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".trace", delete=False)
    f.write(text)
    f.close()
    return f.name


class TestReplay(unittest.TestCase):
    def tearDown(self):
        for name in getattr(self, "_paths", []):
            os.unlink(name)

    def _parse(self, text):
        path = _write(text)
        self._paths = getattr(self, "_paths", []) + [path]
        return parse(path)

    def test_committed_cost_and_hit(self):
        loads, stores = self._parse(V1_TRACE)
        res = replay(loads, stores, PerWordOracle())
        self.assertEqual(res.committed, 2)
        self.assertEqual(res.squashed, 1)
        self.assertEqual(res.unresolved, 0)
        self.assertEqual(res.loads, 2)
        self.assertEqual(res.hits, 1)
        # load 1: safe 50, issue 10, latency 100 -> cost 40
        # load 2: safe 350, issue 310, latency 100 -> cost 40
        self.assertEqual(res.weight, 80)
        self.assertEqual(res.weight_hits, 40)

    def test_squashed_excluded(self):
        loads, stores = self._parse(V1_TRACE)
        res = replay(loads, stores, PerWordOracle())
        squashed = [l for l in loads.values() if l.squashed]
        self.assertEqual(len(squashed), 1)
        self.assertNotIn(squashed[0].seq, [l.seq for l, _ in res.results])

    def test_v0_compat(self):
        loads, stores = self._parse(V0_TRACE)
        res = replay(loads, stores, PerWordOracle())
        self.assertEqual(res.loads, 2)
        self.assertEqual(res.hits, 1)
        self.assertEqual(res.weight, 200)
        self.assertEqual(res.weight_hits, 100)

    def test_causal_store(self):
        # The store at 300 is before the second query at 410; the first
        # query at 110 is before the store.
        loads, stores = self._parse(V1_TRACE)
        res = replay(loads, stores, PerWordOracle())
        first, second = [l for l, _ in res.results]
        self.assertFalse(first.value == 64 and first.addr == 0x10000 and
                         first.seq == 2)
        self.assertEqual(second.seq, 2)


if __name__ == "__main__":
    unittest.main()
