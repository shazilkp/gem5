from m5.objects.SimObject import SimObject
from m5.params import *


class LoadClassificationTable(SimObject):
    """Load Classification Table (LCT) for LVP.
    PC-indexed saturating counters classify loads as
    Unpredictable/Predictable/Constant per the FLOP paper."""

    type = "LoadClassificationTable"
    cxx_class = "gem5::lvp::LoadClassificationTable"
    cxx_header = "cpu/lvp/load_classification_table.hh"

    localPredictorSize = Param.Unsigned(
        128, "Number of entries in the LCT (must be power of 2)"
    )
    localCtrBits = Param.Unsigned(
        8, "Bits per saturating counter (8 for ~240-load threshold)"
    )
    resetOnConstMiss = Param.Bool(
        True, "Reset counter to 0 on constant-class misprediction"
    )


class LoadValuePredictionTable(SimObject):
    """Load Value Prediction Table (LVPT) for LVP.
    Direct-mapped, PC-indexed table storing last-seen values."""

    type = "LoadValuePredictionTable"
    cxx_class = "gem5::lvp::LoadValuePredictionTable"
    cxx_header = "cpu/lvp/load_value_prediction_table.hh"

    entries = Param.Unsigned(
        1024, "Number of LVPT entries (must be power of 2)"
    )


class ConstantVerificationUnit(SimObject):
    """Constant Verification Unit (CVU) for LVP.
    CAM tracking constant loads; stores invalidate matching entries.
    FIFO replacement per FLOP paper §4.2."""

    type = "ConstantVerificationUnit"
    cxx_class = "gem5::lvp::ConstantVerificationUnit"
    cxx_header = "cpu/lvp/constant_verification_unit.hh"

    entries = Param.Unsigned(
        72, "Number of CAM entries (72 per FLOP paper)"
    )


class LoadValuePredictionUnit(SimObject):
    """Top-level Load Value Prediction Unit.
    Coordinates LCT, LVPT, and CVU to model the Apple M3 LVP
    as described in the FLOP paper."""

    type = "LoadValuePredictionUnit"
    cxx_class = "gem5::lvp::LoadValuePredictionUnit"
    cxx_header = "cpu/lvp/load_value_prediction_unit.hh"

    enabled = Param.Bool(True, "Enable the LVP")

    predictorType = Param.String(
        "flop", "LVP model: 'flop' (Apple M3 constant LVP) or 'egdiff' "
        "(ISCA 2026 global value predictor)"
    )

    # ---- EgDiff (global value predictor) parameters ----
    egdiffTableEntries = Param.Unsigned(
        4096, "EgDiff prediction table entries (must be a power of 2)"
    )
    egdiffSeed = Param.Unsigned(
        1, "EgDiff deterministic PRNG seed (FPC / allocation)"
    )
    egdiffColdDeterministicAlloc = Param.Bool(
        False,
        "Allocate a cold u==0 entry with probability 1.0 instead of 1/16 "
        "(Layer-1 unit testing convenience; NOT the modelled hardware)"
    )

    egdiffLatency = Param.Unsigned(
        0,
        "EgDiff normal prediction latency in cycles (paper: 3). The value is "
        "computed at dispatch but only written to the destination register, "
        "marked ready, and used to wake dependents after this many cycles. "
        "Zero preserves the historical immediate-publish behaviour. "
        "Supported range: 0..16."
    )
    egdiffDeferredLatency = Param.Unsigned(
        0,
        "EgDiff deferred-prediction latency in cycles after the base value "
        "becomes available (paper: 2). Zero is immediate."
    )

    lct = Param.LoadClassificationTable(
        LoadClassificationTable(), "Load Classification Table"
    )
    lvpt = Param.LoadValuePredictionTable(
        LoadValuePredictionTable(), "Load Value Prediction Table"
    )
    cvu = Param.ConstantVerificationUnit(
        ConstantVerificationUnit(), "Constant Verification Unit"
    )
