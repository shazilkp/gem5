/*
 * EgDiff Predictor — Global Load Value Prediction (ISCA 2026)
 *
 * "Revisiting Global Value Prediction: A Resurgent Complement to Local
 *  Predictors".
 *
 * This class is a plain C++ object (not a SimObject) so that the prediction
 * table, the Global Value Queue and the commit-stage distance-polling engine
 * can be exercised in isolation.  It is owned by LoadValuePredictionUnit.
 *
 * Hardware modelled (Sections III-A .. III-D, Table I, Algorithm 1):
 *   - 4K-entry, 88-bit Prediction Table:
 *        14b tag | 2b u | 3b FPC ctr | 64b diff | 5b distance
 *   - 3-bit Forward Probabilistic Counter with probability vector
 *        {1, 1/4, 1/4, 1/8, 1/8, 1/8, 1/8}
 *   - 10-bit TICK counter decaying every entry's u bit periodically
 *   - 1/16 allocation probability on a tag mismatch when u == 0
 *   - 32-entry Non-Speculative Value Queue (NVQ) of committed load values
 *   - Commit-stage Distance Polling Algorithm (Algorithm 1)
 *   - 32-entry Speculative GVQ with `Wake` / `Avail` deferred prediction
 *   - Global `lastMisp` suppression window (1024 instructions)
 *
 * Nothing here depends on the O3CPU headers: in-flight instructions are
 * referenced only by their sequence number and an opaque pointer so that the
 * gem5 glue can recover the DynInst when a deferred prediction fires.
 */

#ifndef __CPU_LVP_EGDIFF_PREDICTOR_HH__
#define __CPU_LVP_EGDIFF_PREDICTOR_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

#include "base/types.hh"

namespace gem5
{
namespace lvp
{

/** One 88-bit Prediction Table entry. */
struct EgDiffTableEntry
{
    uint16_t tag = 0;      // 14-bit PC tag (orthogonal hash)
    uint8_t u = 0;         // 2-bit usefulness counter (0..3)
    uint8_t ctr = 0;       // 3-bit FPC (0..7)
    uint64_t diff = 0;     // 64-bit modular stride
    uint8_t distance = 32; // 5-bit relative load distance (1..32)
};

/** One in-flight Speculative GVQ entry (141 bits in hardware). */
struct EgDiffSpecEntry
{
    uint64_t seqNum = 0;    // ROB / LSQ sequence number
    Addr pc = 0;            // PC of the load
    uint64_t value = 0;     // 64-bit value (prediction or actual)
    uint8_t distance = 0;   // 5-bit distance used for this prediction
    uint64_t diff = 0;      // 64-bit diff used for this prediction
    bool vp = false;        // a valid prediction has been written
    bool misp = false;      // verification detected a mismatch
    bool avail = false;     // the actual load has completed in the LQ
    uint64_t wakeSeqNum = 0;// 0 = unbound; else seqNum of deferred dependent
    void *inst = nullptr;   // opaque DynInst* (cast by the O3 glue)
};

/** Outcome of a commit-stage update, for tracing and statistics. */
struct EgDiffCommitResult
{
    bool tagMatched = false;
    bool allocated = false;
    bool polled = false;      // Algorithm 1 mismatch branch taken
    bool strideMatched = false;
    bool fpcIncremented = false;
    bool tickDecayed = false;
    uint8_t oldDistance = 0;
    uint8_t newDistance = 0;
    uint64_t oldDiff = 0;
    uint64_t newDiff = 0;
    uint8_t newCtr = 0;
    uint8_t newU = 0;
};

/** Result of a dispatch-time prediction attempt. */
struct EgDiffPredictResult
{
    bool valid = false;       // a value is being forwarded
    bool deferred = false;    // waiting on an unavailable base load
    bool nvqBase = false;     // base came from the committed NVQ (curIdx < d)
    uint64_t value = 0;
    uint8_t distance = 0;
};

/** Result of a load-completion / verification event. */
struct EgDiffCompleteResult
{
    bool ok = true;             // false => value misprediction, caller squashes
    bool verified = false;      // a forward-looking prediction was checked
    bool wakeIgnored = false;   // dependent had already completed (Avail == 1)
    uint64_t wakeIgnoredSeq = 0;// seqNum of that dependent (for tracing)
    EgDiffSpecEntry *wakeTarget = nullptr;  // dependent to wake (deferred)
    EgDiffPredictResult wake;   // value to forward to that dependent
};

class EgDiffPredictor
{
  public:
    static constexpr unsigned ORDER_N = 32;
    static constexpr unsigned MAXCONF = 7;
    static constexpr unsigned MAXU = 3;
    static constexpr unsigned TICK_PERIOD = 1024;
    static constexpr uint64_t LAST_MISP_WINDOW = 1024;

    EgDiffPredictor(unsigned tableEntries, unsigned seed,
                    bool coldDeterministicAlloc);

    /**
     * 12-bit direct index hash.
     *
     * NOTE: this deliberately uses `pc >> 1`, not `pc >> 2`.  RISC-V with the
     * C extension has 2-byte-aligned instructions, and two loads in the same
     * 4-byte word must map to distinct entries; a `pc >> 2` index (as written
     * illustratively in plan.md) makes them indistinguishable and disables
     * prediction for either of them.
     */
    static unsigned indexHash(Addr pc)
    {
        return ((pc >> 1) ^ (pc >> 13)) & 0xFFFu;
    }

    /**
     * 14-bit tag hash (deliberately a different function than indexHash,
     * and sensitive to PC[1] for the same reason as indexHash).
     */
    static uint16_t tagHash(Addr pc)
    {
        return ((pc >> 1) ^ (pc >> 17) ^ (pc >> 29)) & 0x3FFFu;
    }

    // ---- Layer 1: commit-stage engine --------------------------------
    /**
     * Train the predictor at commit.  Runs the TICK decay, Algorithm 1 and
     * pushes the committed value into the NVQ.  This is the only place the
     * Prediction Table is ever written.
     */
    EgDiffCommitResult commitLoad(Addr pc, RegVal actualVal);

    const EgDiffTableEntry &entry(Addr pc) const
    {
        return table[indexHash(pc)];
    }
    const std::array<uint64_t, ORDER_N> &getNVQ() const { return nvq; }

    // ---- Layer 2: speculative queue / prediction ---------------------
    /**
     * Predict a load at dispatch.  Allocates its Speculative GVQ slot and,
     * if the base value is available, returns the forwarded value.  When the
     * base load is still in flight and unresolved, binds a deferred wakeup.
     */
    EgDiffPredictResult dispatchLoad(Addr pc, uint64_t seqNum, void *inst);

    /**
     * Record the completion of a load in the LQ.  Returns ok == false when a
     * prediction mismatched (caller must squash).  Also fires a deferred
     * wakeup for a dependent that was bound to this load as its base.
     */
    EgDiffCompleteResult completeLoad(uint64_t seqNum, RegVal actualVal);

    /** Drop speculative state for all loads younger than the squash bound.
     *  Returns the number of dangling Wake bindings that were cleared. */
    unsigned squashFrom(uint64_t squashNum);

    /** Pop a committed load from the front of the speculative queue. */
    void retireFront(uint64_t seqNum);

    bool predictionSuppressed(uint64_t seqNum) const
    {
        return suppressUntil != 0 && seqNum <= suppressUntil;
    }

    uint64_t getSuppressUntil() const { return suppressUntil; }
    const std::deque<EgDiffSpecEntry> &getSpecGVQ() const { return specGVQ; }
    size_t specGVQSize() const { return specGVQ.size(); }

    /** Flush everything (used between tests / on reset). */
    void reset();

  private:
    bool forwardProb(uint8_t ctr)
    {
        static const double p[7] = {1.0, 0.25, 0.25, 0.125,
                                    0.125, 0.125, 0.125};
        return dist(rng) < p[ctr];
    }

    std::vector<EgDiffTableEntry> table;
    std::array<uint64_t, ORDER_N> nvq{};
    std::deque<EgDiffSpecEntry> specGVQ;

    uint32_t tickCounter = 0;
    uint64_t suppressUntil = 0;

    const bool coldDeterministicAlloc;
    std::mt19937_64 rng;
    std::uniform_real_distribution<double> dist{0.0, 1.0};
};

} // namespace lvp
} // namespace gem5

#endif // __CPU_LVP_EGDIFF_PREDICTOR_HH__
