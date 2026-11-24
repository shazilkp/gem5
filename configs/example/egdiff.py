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
from m5.objects import StridePrefetcher  # noqa: E402

PAPER_CONFIG = os.environ.get("EGDIFF_PAPER_CONFIG", "0") not in (
    "0", "false", "no")
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
    if PAPER_CONFIG:
        # ISCA 2026 Table II.  The ISA remains RISC-V (the paper uses ARMv8);
        # the SPEC window remains 30M from _start (the paper uses SimPoints).
        options.l3cache = True
        options.l1i_size = "32KiB"
        options.l1i_assoc = 4
        options.l1d_size = "32KiB"
        options.l1d_assoc = 4
        options.l2_size = "256KiB"
        options.l2_assoc = 8
        options.l3_size = "2MiB"
        options.l3_assoc = 16
        options.bp_type = "LTAGE"

    _real_config_cache(options, system)

    if PAPER_CONFIG:
        system.cpu_clk_domain.clock = "4GHz"
        system.clk_domain.clock = "4GHz"
        for cpu in system.cpu:
            cpu.fetchWidth = 8
            cpu.decodeWidth = 8
            cpu.dispatchWidth = 8
            cpu.issueWidth = 8
            cpu.commitWidth = 8
            cpu.wbWidth = 8
            cpu.numPhysIntRegs = 168
            cpu.numPhysFloatRegs = 168
            cpu.numROBEntries = 192
            for iq in cpu.instQueues:
                iq.numEntries = 64
            cpu.LQEntries = 32
            cpu.SQEntries = 32
            if cpu.dcache:
                cpu.dcache.tag_latency = 1
                cpu.dcache.data_latency = 1
                cpu.dcache.response_latency = 1
                cpu.dcache.prefetcher = StridePrefetcher(degree=4)
            if cpu.icache:
                cpu.icache.tag_latency = 1
                cpu.icache.data_latency = 1
                cpu.icache.response_latency = 0
        if getattr(system, "l2", None):
            system.l2.tag_latency = 4
            system.l2.data_latency = 4
            system.l2.response_latency = 4
        if getattr(system, "l3", None):
            system.l3.tag_latency = 11
            system.l3.data_latency = 10
            system.l3.response_latency = 11
        print("[egdiff] PAPER_CONFIG: 4GHz, L1 32K/4-way, L2 256K/8-way, "
              "L3 2M/16-way, L-TAGE, 168 regs, ROB/IQ/LQ/SQ 192/64/32/32")

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
