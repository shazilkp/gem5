/*
 * Load Value Prediction Unit (LVPU)
 * Top-level controller coordinating LCT + LVPT + CVU.
 * Models the Apple M3 LVP as reverse-engineered in the FLOP paper.
 *
 * Key FLOP-paper behaviours modelled:
 *   - 8-byte loads: only predict zero (§4.2)
 *   - ~240-load training threshold via 8-bit saturating counters
 *   - 72-entry CVU for simultaneous tracking
 *   - PC-tagged, constant-only training (no stride)
 *   - FIFO replacement
 */

#ifndef __CPU_LVP_LOAD_VALUE_PREDICTION_UNIT_HH__
#define __CPU_LVP_LOAD_VALUE_PREDICTION_UNIT_HH__

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/lvp/constant_verification_unit.hh"
#include "cpu/lvp/egdiff_predictor.hh"
#include "cpu/lvp/load_classification_table.hh"
#include "cpu/lvp/load_value_prediction_table.hh"
#include "cpu/lvp/lvp_enums.hh"
#include "params/LoadValuePredictionUnit.hh"
#include "sim/sim_object.hh"

#include <memory>
#include <string>

namespace gem5
{
namespace lvp
{

/**
 * Result of a load prediction attempt.
 */
struct LVPResult
{
    LVPClassification classification;
    RegVal predictedValue;
    bool valid;
};

class LoadValuePredictionUnit : public SimObject
{
  public:
    PARAMS(LoadValuePredictionUnit);
    LoadValuePredictionUnit(const Params &p);
    ~LoadValuePredictionUnit() = default;

    /**
     * Predict the value for a load instruction.
     * @param tid        Thread ID.
     * @param instPC     PC of the load instruction.
     * @param loadSize   Size of the load in bytes.
     * @return LVPResult containing classification, value, and validity.
     */
    LVPResult predictLoad(ThreadID tid, Addr instPC, unsigned loadSize);

    /**
     * Verify a prediction after the load completes.
     * Updates LCT, LVPT, and triggers CVU invalidation on mismatch.
     *
     * @param tid           Thread ID.
     * @param instPC        PC of the load.
     * @param loadAddr      Effective address of the load.
     * @param correctVal    The actual value loaded from memory.
     * @param predictedVal  The value that was predicted.
     * @param prediction    The classification used for prediction.
     * @return true if the prediction was correct.
     */
    bool verifyPrediction(ThreadID tid, Addr instPC, Addr loadAddr,
                          RegVal correctVal, RegVal predictedVal,
                          LVPClassification prediction);

    /**
     * Train the LVP for a load that was not predicted.
     * Looks up the last value in LVPT to determine if the value has changed.
     */
    void trainLoad(ThreadID tid, Addr instPC, RegVal actualVal);

    /**
     * Notify the CVU of a store's effective address.
     */
    void processStoreAddress(ThreadID tid, Addr storeAddr);

    /**
     * Notify the CVU of a constant load address for tracking.
     */
    void processConstLoadAddress(ThreadID tid, Addr instPC,
                                 Addr loadAddr, unsigned lvptIndex);

    /**
     * Get the LVPT index for an instruction PC.
     */
    unsigned getLVPTIndex(Addr instPC, ThreadID tid);

    /** Whether LVP is enabled. */
    bool isEnabled() const { return enabled; }

    /** Whether the EgDiff global predictor (rather than FLOP) is selected. */
    bool isEgDiff() const { return predictorType == "egdiff"; }

    /** Configured EgDiff prediction latencies (cycles; 0 = immediate). */
    unsigned egdiffLatencyCycles() const { return egdiffLatency; }
    unsigned egdiffDeferredLatencyCycles() const
    {
        return egdiffDeferredLatency;
    }

    // =================================================================
    //  EgDiff (ISCA 2026) glue.  All in-flight instructions are referred
    //  to by (seqNum, opaque DynInst*) so that this header stays free of
    //  the O3CPU headers.
    // =================================================================

    /**
     * Commit-stage update (Layer 1): run Algorithm 1 (distance polling),
     * the TICK decay and push the committed value into the NVQ.  The
     * Prediction Table is written only here.
     */
    void commitLoad(uint64_t seqNum, Addr instPC, RegVal actualVal);

    /**
     * Dispatch-stage prediction (Layer 2/3): allocate a Speculative GVQ
     * slot and, if the base value is ready, forward a predicted value.
     */
    EgDiffPredictResult dispatchLoad(uint64_t seqNum, Addr instPC,
                                     void *inst);

    /**
     * Load-completion hook (Layer 2/3): record the actual value, verify
     * any prediction, and fire a deferred wakeup if this load is a bound
     * base.  Returns ok == false when the caller must squash.
     *
     * @param wakeInst   [out] DynInst* of the dependent that must be woken
     *                   (nullptr if none).
     * @param wakeResult [out] prediction value to forward to that dependent.
     */
    bool completeLoad(uint64_t seqNum, RegVal actualVal,
                      void *&wakeInst, EgDiffPredictResult &wakeResult);

    /** Drop speculative state younger than a squash bound (ROB squash). */
    void squashFrom(uint64_t squashNum);

    /** Pop a committed load from the front of the speculative queue. */
    void retireLoad(uint64_t seqNum);

    /** Accessor for tracing. */
    const EgDiffPredictor *egdiff() const { return egdiffPredictor.get(); }

  private:
    /** Check if the load is eligible for prediction per FLOP rules. */
    bool isEligible(unsigned loadSize, RegVal value) const;

    LoadClassificationTable *lct;
    LoadValuePredictionTable *lvpt;
    ConstantVerificationUnit *cvu;

    const bool enabled;
    const std::string predictorType;
    const unsigned egdiffLatency;
    const unsigned egdiffDeferredLatency;
    std::unique_ptr<EgDiffPredictor> egdiffPredictor;

    struct LVPStats : public statistics::Group
    {
        LVPStats(LoadValuePredictionUnit *lvpu);
        statistics::Scalar totalLoads;
        statistics::Scalar numPredictions;
        statistics::Scalar numConstLoads;
        statistics::Scalar numPredictionCorrect;
        statistics::Scalar numPredictionIncorrect;
        statistics::Scalar numTrainCorrect;
        statistics::Scalar numTrainIncorrect;
        statistics::Scalar num8ByteZeroPredictions;
        statistics::Scalar numSuppressed;

        // ---- EgDiff statistics ----
        statistics::Scalar egdiffCommits;
        statistics::Scalar egdiffCommittedPredicted;
        statistics::Scalar egdiffDispatchedLoads;
        statistics::Scalar egdiffTableAllocations;
        statistics::Scalar egdiffPollingSteps;
        statistics::Scalar egdiffStrideMatches;
        statistics::Scalar egdiffFpcIncrements;
        statistics::Scalar egdiffTickDecays;
        statistics::Scalar egdiffPredictions;
        statistics::Scalar egdiffDeferredBindings;
        statistics::Scalar egdiffDeferredPredictions;
        statistics::Scalar egdiffDeferredIgnored;
        statistics::Scalar egdiffNVQBases;
        statistics::Scalar egdiffWakeCleared;
        statistics::Scalar egdiffLastMispSuppressed;
        statistics::Scalar egdiffSquashed; 
    } stats;
};

} // namespace lvp
} // namespace gem5

#endif // __CPU_LVP_LOAD_VALUE_PREDICTION_UNIT_HH__
