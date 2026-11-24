/*
 * Load Value Prediction Unit (LVPU) — Implementation
 */

#include "cpu/lvp/load_value_prediction_unit.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/EgDiff.hh"
#include "debug/LVP.hh"

namespace gem5
{
namespace lvp
{

LoadValuePredictionUnit::LoadValuePredictionUnit(const Params &p)
    : SimObject(p),
      lct(p.lct),
      lvpt(p.lvpt),
      cvu(p.cvu),
      enabled(p.enabled),
      predictorType(p.predictorType),
      egdiffLatency(p.egdiffLatency),
      egdiffDeferredLatency(p.egdiffDeferredLatency),
      stats(this)
{
    if (predictorType == "egdiff") {
        egdiffPredictor = std::make_unique<EgDiffPredictor>(
            p.egdiffTableEntries, p.egdiffSeed,
            p.egdiffColdDeterministicAlloc);
        if (egdiffLatency > 16) {
            fatal("egdiffLatency=%u exceeds the supported modelling range "
                  "(0..16 cycles)", egdiffLatency);
        }
        DPRINTF(EgDiff, "EgDiff predictor created: %u table entries, "
                "order n=%u, seed=%u, coldDeterministicAlloc=%d\n",
                p.egdiffTableEntries, EgDiffPredictor::ORDER_N,
                p.egdiffSeed, p.egdiffColdDeterministicAlloc);
    } else if (predictorType != "flop") {
        fatal("LoadValuePredictionUnit: unknown predictorType '%s' "
              "(expected 'flop' or 'egdiff')", predictorType);
    }

    DPRINTF(LVP, "LVP Unit created: enabled=%d type=%s\n",
            enabled, predictorType);
}

LoadValuePredictionUnit::LVPStats::LVPStats(LoadValuePredictionUnit *lvpu)
    : statistics::Group(lvpu),
      ADD_STAT(totalLoads, statistics::units::Count::get(),
               "Total loads seen by LVP"),
      ADD_STAT(numPredictions, statistics::units::Count::get(),
               "Loads where a prediction was made"),
      ADD_STAT(numConstLoads, statistics::units::Count::get(),
               "Loads classified as constant"),
      ADD_STAT(numPredictionCorrect, statistics::units::Count::get(),
               "Correct speculative LVP predictions"),
      ADD_STAT(numPredictionIncorrect, statistics::units::Count::get(),
               "Incorrect speculative LVP predictions (squash required)"),
      ADD_STAT(numTrainCorrect, statistics::units::Count::get(),
               "Unpredicted loads matching LVPT (training hits)"),
      ADD_STAT(numTrainIncorrect, statistics::units::Count::get(),
               "Unpredicted loads mismatching LVPT (training misses)"),
      ADD_STAT(num8ByteZeroPredictions, statistics::units::Count::get(),
               "8-byte loads where zero was predicted"),
      ADD_STAT(numSuppressed, statistics::units::Count::get(),
               "Predictions suppressed (8-byte non-zero)"),
      ADD_STAT(egdiffCommits, statistics::units::Count::get(),
               "EgDiff: committed loads trained at the commit stage"),
      ADD_STAT(egdiffCommittedPredicted, statistics::units::Count::get(),
               "EgDiff: committing loads whose prediction verified"),
      ADD_STAT(egdiffDispatchedLoads, statistics::units::Count::get(),
               "EgDiff: loads dispatched as speculative-queue candidates"),
      ADD_STAT(egdiffTableAllocations, statistics::units::Count::get(),
               "EgDiff: prediction-table entries allocated (tag mismatch)"),
      ADD_STAT(egdiffPollingSteps, statistics::units::Count::get(),
               "EgDiff: Algorithm-1 distance-polling mismatch steps"),
      ADD_STAT(egdiffStrideMatches, statistics::units::Count::get(),
               "EgDiff: Algorithm-1 stride matches at the current distance"),
      ADD_STAT(egdiffFpcIncrements, statistics::units::Count::get(),
               "EgDiff: forward-probabilistic-counter increments"),
      ADD_STAT(egdiffTickDecays, statistics::units::Count::get(),
               "EgDiff: 10-bit TICK usefulness decay events"),
      ADD_STAT(egdiffPredictions, statistics::units::Count::get(),
               "EgDiff: values forwarded at dispatch (normal prediction)"),
      ADD_STAT(egdiffDeferredBindings, statistics::units::Count::get(),
               "EgDiff: deferred-prediction wake bindings created"),
      ADD_STAT(egdiffDeferredPredictions, statistics::units::Count::get(),
               "EgDiff: deferred predictions fired on base completion"),
      ADD_STAT(egdiffDeferredIgnored, statistics::units::Count::get(),
               "EgDiff: deferred wakeups discarded (dependent already done)"),
      ADD_STAT(egdiffNVQBases, statistics::units::Count::get(),
               "EgDiff: predictions whose base came from the committed NVQ"),
      ADD_STAT(egdiffWakeCleared, statistics::units::Count::get(),
               "EgDiff: Wake bindings cleared because the target was squashed"),
      ADD_STAT(egdiffLastMispSuppressed, statistics::units::Count::get(),
               "EgDiff: predictions suppressed by the 1024-inst lastMisp"),
      ADD_STAT(egdiffSquashed, statistics::units::Count::get(),
               "EgDiff: speculative entries discarded by ROB squash")
{
}

bool
LoadValuePredictionUnit::isEligible(unsigned loadSize, RegVal value) const
{
    // Per FLOP paper §4.2: 8-byte loads are only predicted if
    // the value is zero.  Loads ≤4 bytes are always eligible.
    // loadSize==0 means size is unknown (dispatch time); allow it
    // and defer the check to verification.
    if (loadSize == 0 || loadSize <= 4)
        return true;
    if (loadSize == 8 && value == 0)
        return true;
    return false;
}

LVPResult
LoadValuePredictionUnit::predictLoad(ThreadID tid, Addr instPC,
                                     unsigned loadSize)
{
    LVPResult result;
    result.classification = LVPClassification::StrongUnpredictable;
    result.predictedValue = 0;
    result.valid = false;

    if (!enabled)
        return result;

    // EgDiff predictions are issued by dispatchLoad() (it needs the DynInst
    // sequence number and the speculative GVQ), not by this FLOP entry point.
    if (isEgDiff())
        return result;

    ++stats.totalLoads;

    // Step 1: Classification lookup
    LVPClassification cls = lct->lookup(tid, instPC);
    result.classification = cls;

    // Only make predictions for Constant-classified loads
    if (cls != LVPClassification::Constant) {
        DPRINTF(LVP, "predictLoad: PC %#x class=%d, not constant\n",
                instPC, (int)cls);
        return result;
    }

    ++stats.numConstLoads;

    // Step 2: Value lookup
    bool lvptValid = false;
    RegVal predicted = lvpt->lookup(tid, instPC, &lvptValid);

    if (!lvptValid) {
        DPRINTF(LVP, "predictLoad: PC %#x no LVPT entry\n", instPC);
        return result;
    }

    // Step 3: Apply FLOP 8-byte guard
    if (!isEligible(loadSize, predicted)) {
        DPRINTF(LVP, "predictLoad: PC %#x suppressed (8B non-zero: %#x)\n",
                instPC, predicted);
        ++stats.numSuppressed;
        return result;
    }

    if (loadSize == 8 && predicted == 0) {
        ++stats.num8ByteZeroPredictions;
    }

    result.predictedValue = predicted;
    result.valid = true;
    ++stats.numPredictions;

    DPRINTF(LVP, "predictLoad: PC %#x → val %#x (constant, %dB)\n",
            instPC, predicted, loadSize);

    return result;
}

bool
LoadValuePredictionUnit::verifyPrediction(ThreadID tid, Addr instPC,
                                          Addr loadAddr, RegVal correctVal,
                                          RegVal predictedVal,
                                          LVPClassification prediction)
{
    if (!enabled)
        return true;

    bool correct = (correctVal == predictedVal);

    // Update LVPT with the actual value (always)
    lvpt->update(instPC, correctVal, tid);

    // Update LCT
    lct->update(tid, instPC, prediction, correct);

    if (correct) {
        DPRINTF(LVP, "verifyPrediction: PC %#x CORRECT (val %#x)\n",
                instPC, correctVal);
        ++stats.numPredictionCorrect;
    } else {
        DPRINTF(LVP, "verifyPrediction: PC %#x MISMATCH "
                "(predicted %#x, actual %#x)\n",
                instPC, predictedVal, correctVal);
        ++stats.numPredictionIncorrect;
    }

    return correct;
}

void
LoadValuePredictionUnit::trainLoad(ThreadID tid, Addr instPC, RegVal actualVal)
{
    if (!enabled)
        return;

    bool lvptValid = false;
    RegVal prevVal = lvpt->lookup(tid, instPC, &lvptValid);

    bool correct = false;
    if (lvptValid && prevVal == actualVal) {
        correct = true;
    }

    // Always update LVPT with actual value
    lvpt->update(instPC, actualVal, tid);

    // Update LCT based on whether value matches previous
    lct->update(tid, instPC, LVPClassification::StrongUnpredictable, correct);

    if (correct) {
        DPRINTF(LVP, "trainLoad: PC %#x CORRECT (val %#x)\n",
                instPC, actualVal);
        ++stats.numTrainCorrect;
    } else {
        DPRINTF(LVP, "trainLoad: PC %#x MISMATCH "
                "(prev %#x, actual %#x)\n",
                instPC, prevVal, actualVal);
        ++stats.numTrainIncorrect;
    }
}

void
LoadValuePredictionUnit::processStoreAddress(ThreadID tid, Addr storeAddr)
{
    if (!enabled)
        return;

    // The CVU only exists to invalidate FLOP constant predictions on a
    // matching store; EgDiff re-verifies every load at completion and so
    // needs no store-side invalidation.
    if (isEgDiff())
        return;

    cvu->processStoreAddress(tid, storeAddr);
}

// ==========================================================================
//  EgDiff glue
// ==========================================================================

void
LoadValuePredictionUnit::commitLoad(uint64_t seqNum, Addr instPC,
                                    RegVal actualVal)
{
    if (!egdiffPredictor)
        return;

    // Architectural accounting for coverage/accuracy: a prediction only counts
    // if the load it belongs to actually commits.  Counting at dispatch is
    // wrong on branchy code, where wrong-path loads are predicted and then
    // squashed, inflating both the numerator and the denominator.
    const auto &spec = egdiffPredictor->getSpecGVQ();
    if (!spec.empty() && spec.front().seqNum == seqNum && spec.front().vp)
        ++stats.egdiffCommittedPredicted;

    const EgDiffCommitResult r =
        egdiffPredictor->commitLoad(instPC, seqNum, actualVal);

    ++stats.egdiffCommits;
    if (r.allocated)
        ++stats.egdiffTableAllocations;
    if (r.polled)
        ++stats.egdiffPollingSteps;
    if (r.strideMatched)
        ++stats.egdiffStrideMatches;
    if (r.fpcIncremented)
        ++stats.egdiffFpcIncrements;
    if (r.tickDecayed)
        ++stats.egdiffTickDecays;

    DPRINTF(EgDiff, "[sn:%llu] commit PC %#x val %#x idx %u tag %#x | "
            "match=%d alloc=%d poll=%d stride=%d fpc=%d | "
            "d %u->%u diff %#x->%#x ctr=%u u=%u tickDecay=%d\n",
            seqNum, instPC, actualVal, EgDiffPredictor::indexHash(instPC),
            EgDiffPredictor::tagHash(instPC), r.tagMatched, r.allocated,
            r.polled, r.strideMatched, r.fpcIncremented, r.oldDistance,
            r.newDistance, r.oldDiff, r.newDiff, r.newCtr, r.newU,
            r.tickDecayed);

    // The load is now committed: it may leave the speculative queue.
    egdiffPredictor->retireFront(seqNum);
}

EgDiffPredictResult
LoadValuePredictionUnit::dispatchLoad(uint64_t seqNum, Addr instPC,
                                      void *inst)
{
    if (!egdiffPredictor)
        return EgDiffPredictResult();

    const EgDiffPredictResult r =
        egdiffPredictor->dispatchLoad(instPC, seqNum, inst);

    // Count only loads that would actually have produced a prediction; do not
    // count every load dispatched during the window, and do not double-count
    // skid-buffer re-dispatches (dispatchLoad is idempotent by seqNum).
    if (r.suppressed)
        ++stats.egdiffLastMispSuppressed;

    if (r.allocated)
        ++stats.egdiffDispatchedLoads;

    if (r.valid) {
        ++stats.egdiffPredictions;
        if (r.nvqBase)
            ++stats.egdiffNVQBases;
        DPRINTF(EgDiff, "[sn:%llu] dispatch PC %#x predicts %#x "
                "(d=%u, base=%s, base_pc=%#x, base_val=%#x)\n",
                seqNum, instPC, r.value, r.distance,
                r.nvqBase ? "nvq" : "specgvq", r.basePc, r.baseVal);
    } else if (r.deferred) {
        ++stats.egdiffDeferredBindings;
        DPRINTF(EgDiff, "[sn:%llu] dispatch PC %#x deferred (d=%u)\n",
                seqNum, instPC, r.distance);
    }

    return r;
}

bool
LoadValuePredictionUnit::completeLoad(uint64_t seqNum, RegVal actualVal,
                                      void *&wakeInst,
                                      EgDiffPredictResult &wakeResult)
{
    wakeInst = nullptr;
    wakeResult = EgDiffPredictResult();

    if (!egdiffPredictor)
        return true;

    const EgDiffCompleteResult r = egdiffPredictor->completeLoad(seqNum,
                                                                 actualVal);

    if (r.verified) {
        if (!r.ok) {
            ++stats.numPredictionIncorrect;
            DPRINTF(EgDiff, "[tick:%llu] [sn:%llu] MISMATCH actual %#x -> "
                    "squash; lastMisp window starts at the squash\n",
                    curTick(), seqNum, actualVal);
        } else {
            ++stats.numPredictionCorrect;
            DPRINTF(EgDiff, "[sn:%llu] verified actual %#x\n",
                    seqNum, actualVal);
        }
    } else {
        DPRINTF(EgDiff, "[sn:%llu] completion actual %#x (no prediction)\n",
                seqNum, actualVal);
    }

    if (r.wakeTarget) {
        ++stats.egdiffDeferredPredictions;
        wakeInst = r.wakeTarget->inst;
        wakeResult = r.wake;
        DPRINTF(EgDiff, "[sn:%llu] deferred wakeup -> publish %#x to "
                "dependent PC %#x [dep:%llu] (d=%u, base_pc=%#x, "
                "base_val=%#x)\n", seqNum, r.wake.value,
                r.wakeTarget->pc, (unsigned long long)r.wakeTarget->seqNum,
                r.wakeTarget->distance, r.wake.basePc, r.wake.baseVal);
    } else if (r.wakeIgnored) {
        ++stats.egdiffDeferredIgnored;
        DPRINTF(EgDiff, "[sn:%llu] deferred wakeup ignored "
                "(dependent [dep:%llu] already completed)\n",
                seqNum, (unsigned long long)r.wakeIgnoredSeq);
    }

    return r.ok;
}

void
LoadValuePredictionUnit::dispatchInstruction()
{
    if (egdiffPredictor)
        egdiffPredictor->dispatchInstruction();
}

void
LoadValuePredictionUnit::publishDeferred(uint64_t seqNum)
{
    if (!egdiffPredictor)
        return;

    DPRINTF(EgDiff, "[tick:%llu] [sn:%llu] deferred prediction published\n",
            curTick(), seqNum);
    egdiffPredictor->publishDeferred(seqNum);
}

void
LoadValuePredictionUnit::squashFrom(uint64_t squashNum)
{
    if (!egdiffPredictor)
        return;

    const size_t before = egdiffPredictor->specGVQSize();
    const unsigned cleared = egdiffPredictor->squashFrom(squashNum);
    const size_t after = egdiffPredictor->specGVQSize();
    if (before != after)
        stats.egdiffSquashed += (before - after);
    if (cleared)
        stats.egdiffWakeCleared += cleared;

    DPRINTF(EgDiff, "squashFrom sn:%llu dropped %u speculative entries, "
            "cleared %u wake bindings\n", squashNum,
            (unsigned)(before - after), cleared);
}

void
LoadValuePredictionUnit::retireLoad(uint64_t seqNum)
{
    if (egdiffPredictor)
        egdiffPredictor->retireFront(seqNum);
}

void
LoadValuePredictionUnit::processConstLoadAddress(ThreadID tid, Addr instPC,
                                                 Addr loadAddr,
                                                 unsigned lvptIndex)
{
    if (!enabled)
        return;

    cvu->updateConstLoad(instPC, loadAddr, (Addr)lvptIndex, tid);
}

unsigned
LoadValuePredictionUnit::getLVPTIndex(Addr instPC, ThreadID tid)
{
    return lvpt->getIndex(instPC, tid);
}

} // namespace lvp
} // namespace gem5
