# EgDiff (ISCA 2026) SE-mode launcher.
#
# Runs a statically linked binary under the standard ``se.py`` script but
# selects the EgDiff global load-value predictor on every O3 CPU.
#
# Usage:
#   build/RISCV/gem5.opt configs/example/egdiff.py \
#       --cpu-type=O3CPU --caches --cmd=<elf> [se.py options...]
#
# Environment:
#   EGDIFF_TYPE        predictor model, "egdiff" (default) or "flop"
#   EGDIFF_ON          0 disables the predictor entirely (default 1)
#   EGDIFF_TABLE       prediction-table entries (default 4096)
#   EGDIFF_SEED        deterministic PRNG seed (default 1)
#   EGDIFF_COLD_ALLOC  1 allocates cold u==0 entries deterministically
#                      instead of with probability 1/16 (unit-test only)

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_CONFIGS = os.path.normpath(os.path.join(_HERE, ".."))
sys.path.insert(0, _CONFIGS)

from common import CacheConfig  # noqa: E402

PRED_TYPE = os.environ.get("EGDIFF_TYPE", "egdiff")
PRED_ON = os.environ.get("EGDIFF_ON", "1") not in ("0", "false", "no")
TABLE = int(os.environ.get("EGDIFF_TABLE", "4096"))
SEED = int(os.environ.get("EGDIFF_SEED", "1"))
COLD_ALLOC = os.environ.get("EGDIFF_COLD_ALLOC", "0") not in (
    "0", "false", "no")
LATENCY = int(os.environ.get("EGDIFF_LATENCY", "0"))
DEFERRED_LATENCY = int(os.environ.get("EGDIFF_DEFERRED_LATENCY", "0"))
NVQ_BASE = os.environ.get("EGDIFF_NVQ_BASE", "1") not in ("0", "false", "no")

_real_config_cache = CacheConfig.config_cache


def _egdiff_config_cache(options, system):
    _real_config_cache(options, system)

    for cpu in system.cpu:
        lvp = cpu.loadValuePredictor
        if lvp is None:
            print("[egdiff] cpu %d has no loadValuePredictor" % cpu.cpu_id)
            continue

        lvp.enabled = PRED_ON
        lvp.predictorType = PRED_TYPE
        lvp.egdiffTableEntries = TABLE
        lvp.egdiffSeed = SEED
        lvp.egdiffColdDeterministicAlloc = COLD_ALLOC
        lvp.egdiffLatency = LATENCY
        lvp.egdiffDeferredLatency = DEFERRED_LATENCY
        lvp.egdiffNvqBase = NVQ_BASE

        print(
            "[egdiff] cpu %d: type=%s enabled=%s entries=%d seed=%d "
            "coldAlloc=%s latency=%d deferredLatency=%d nvqBase=%s"
            % (cpu.cpu_id, PRED_TYPE, PRED_ON, TABLE, SEED, COLD_ALLOC,
               LATENCY, DEFERRED_LATENCY, NVQ_BASE)
        )


CacheConfig.config_cache = _egdiff_config_cache

_SE = os.path.join(_CONFIGS, "deprecated", "example", "se.py")
sys.argv = [_SE] + sys.argv[1:]
with open(_SE) as _f:
    exec(compile(_f.read(), _SE, "exec"), {"__name__": "__main__",
                                           "__file__": _SE})
