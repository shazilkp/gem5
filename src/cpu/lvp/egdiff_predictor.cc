/*
 * EgDiff Predictor — Implementation
 */

#include "cpu/lvp/egdiff_predictor.hh"

#include <algorithm>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/EgDiff.hh"

namespace gem5
{
namespace lvp
{

EgDiffPredictor::EgDiffPredictor(unsigned tableEntries, unsigned seed,
                                 bool coldDeterministicAlloc,
                                 bool nvqBaseAllowed)
    : coldDeterministicAlloc(coldDeterministicAlloc),
      nvqBaseAllowed(nvqBaseAllowed),
      rng(seed)
{
    fatal_if(tableEntries == 0 || (tableEntries & (tableEntries - 1)) != 0,
             "EgDiff: prediction table entries must be a power of 2");
    table.resize(tableEntries);
}

EgDiffCommitResult
EgDiffPredictor::commitLoad(Addr pc, uint64_t seqNum, RegVal actualVal)
{
    EgDiffCommitResult r;

    const unsigned idx = indexHash(pc);
    const uint16_t t = tagHash(pc);
    EgDiffTableEntry &e = table[idx];

    // Use the distance/diff captured in this load's speculative-queue entry at
    // dispatch.  The live table entry may already have been updated by an
    // earlier instance of the same PC; the paper's Algorithm 1 compares against
    // the stride stored in the value-queue entry (Section III-A, Table I).
    auto qit = std::find_if(specGVQ.begin(), specGVQ.end(),
                            [seqNum](const EgDiffSpecEntry &x) {
                                return x.seqNum == seqNum;
                            });
    const bool haveSnapshot = (qit != specGVQ.end() && qit->snapshot);

    // ---- 10-bit TICK usefulness decay (Section III-B) ----
    if (++tickCounter >= TICK_PERIOD) {
        tickCounter = 0;
        for (auto &x : table) {
            if (x.u > 0)
                --x.u;
        }
        r.tickDecayed = true;
    }

    if (e.tag == t) {
        // ---------------- Algorithm 1: Distance Polling ----------------
        r.tagMatched = true;

        const uint8_t d = haveSnapshot ? qit->distance : e.distance;
        const uint64_t delta_p = haveSnapshot ? qit->diff : e.diff;
        const uint64_t nvq_d = nvq[d - 1];
        const uint64_t delta_a = actualVal - nvq_d;

        r.oldDistance = d;
        r.oldDiff = delta_p;

        if (delta_a != delta_p) {
            // ------- mismatch: reset confidence, poll distance -------
            e.ctr = 0;
            e.u = 0;
            r.polled = true;

            if (d > 1) {
                e.distance = d - 1;
                e.diff = actualVal - nvq[e.distance - 1];
            } else {
                e.distance = ORDER_N;
                e.diff = actualVal - nvq[ORDER_N - 1];
            }
        } else {
            // ------- match: FPC confidence + usefulness -------
            r.strideMatched = true;

            if (e.ctr < MAXCONF && forwardProb(e.ctr)) {
                ++e.ctr;
                r.fpcIncremented = true;
            }
            if (e.u < MAXU)
                ++e.u;
        }

        r.newDistance = e.distance;
        r.newDiff = e.diff;
        r.newCtr = e.ctr;
        r.newU = e.u;
    } else {
        // ------- tag mismatch: u==0 replacement with p = 1/16 -------
        const bool alloc = (e.u == 0) &&
                           (coldDeterministicAlloc || (rng() % 16 == 0));
        if (alloc) {
            e.tag = t;
            e.u = 0;
            e.ctr = 0;
            e.distance = ORDER_N;
            e.diff = actualVal - nvq[ORDER_N - 1];

            r.allocated = true;
            r.newDistance = e.distance;
            r.newDiff = e.diff;
            r.newCtr = e.ctr;
            r.newU = e.u;
        }
    }

    // ---- Propagate committed value into the NVQ (FIFO) ----
    // nvq[0] is NVQ[1] (most recent), matching the 1-based paper indexing.
    for (unsigned i = ORDER_N - 1; i > 0; --i)
        nvq[i] = nvq[i - 1];
    nvq[0] = actualVal;

    return r;
}

EgDiffPredictResult
EgDiffPredictor::dispatchLoad(Addr pc, uint64_t seqNum, void *inst)
{
    EgDiffPredictResult res;

    // Idempotence: IEW may re-process an instruction from its skid buffer, so
    // a load can be offered for allocation more than once with the same
    // sequence number.  Allocate exactly once.
    for (const auto &existing : specGVQ) {
        if (existing.seqNum == seqNum)
            return res;
    }

    // The Speculative GVQ mirrors the LQ; if it is somehow full we simply do
    // not allocate a slot (no prediction for this load).
    if (specGVQ.size() >= ORDER_N)
        return res;

    const EgDiffTableEntry &e = table[indexHash(pc)];
    const bool tagMatch = (e.tag == tagHash(pc));

    EgDiffSpecEntry q;
    q.seqNum = seqNum;
    q.pc = pc;
    q.inst = inst;

    const size_t curIdx = specGVQ.size();

    if (tagMatch && e.ctr == MAXCONF && predictionSuppressed())
        res.suppressed = true;

    if (!predictionSuppressed() && tagMatch && e.ctr == MAXCONF) {
        // Section III-A / Table I: a predicted load carries the distance and
        // diff that were in force at dispatch.  Commit must use this snapshot,
        // not the live table entry, because an earlier instance of the same PC
        // may commit and mutate the table while this load is in flight.
        // Unpredicted loads keep using the live table for training; using a
        // stale snapshot there starves confidence growth.
        q.distance = e.distance;
        q.diff = e.diff;
        q.snapshot = true;

        const uint8_t d = q.distance;

        bool haveBase = false;
        uint64_t baseVal = 0;

        if (curIdx >= d) {
            // Base load still in flight in the speculative queue.
            const EgDiffSpecEntry &base = specGVQ[curIdx - d];
            // `pending` means the base has completed and its value is known,
            // but its deferred publication is still in flight.  The value is
            // valid to use as a base (and matches the behaviour before the
            // deferred publish was split into a separate stage).
            if (base.avail || base.vp || base.pending) {
                haveBase = true;
                baseVal = base.value;
                res.baseVal = base.value;
                res.basePc = base.pc;
            } else {
                // Bind a deferred wakeup (one-to-one, Section III-C).
                EgDiffSpecEntry &mutableBase = specGVQ[curIdx - d];
                if (mutableBase.wakeSeqNum == 0) {
                    mutableBase.wakeSeqNum = seqNum;
                    res.deferred = true;
                }
            }
        } else if (nvqBaseAllowed) {
            // Base load has already committed: read it from the NVQ.
            // This is the model's Rule C extension.  The paper's prediction
            // text only describes the speculative queue, so strict-fidelity
            // experiments disable this path.
            haveBase = true;
            baseVal = nvq[d - curIdx - 1];
            res.nvqBase = true;
            res.baseVal = baseVal;
        }

        if (haveBase) {
            const uint64_t predVal = baseVal + e.diff;
            q.value = predVal;
            q.vp = true;
            res.valid = true;
            res.value = predVal;
        }
        res.distance = d;
    }

    specGVQ.push_back(q);
    DPRINTF(EgDiff, "alloc sn=%llu pc=%#x\n", seqNum, pc);
    res.allocated = true;
    return res;
}

EgDiffCompleteResult
EgDiffPredictor::completeLoad(uint64_t seqNum, RegVal actualVal)
{
    EgDiffCompleteResult res;

    auto it = std::find_if(specGVQ.begin(), specGVQ.end(),
                           [seqNum](const EgDiffSpecEntry &x) {
                               return x.seqNum == seqNum;
                           });
    if (it == specGVQ.end())
        return res; // untracked load: nothing to verify

    const size_t curIdx = std::distance(specGVQ.begin(), it);
    EgDiffSpecEntry &q = *it;

    if (q.vp) {
        res.verified = true;
        if (q.value != actualVal) {
            // Value misprediction: squash and suppress predictions globally.
            q.misp = true;
            q.value = actualVal;
            q.vp = false;
            q.avail = true;
            // Start the 1024-instruction window when the resulting squash
            // happens, so wrong-path instructions fetched after this load do
            // not consume the window.
            suppressPending = true;
            suppressUntilCount = 0;
            res.ok = false;
        } else {
            q.avail = true;
        }
    } else {
        // No *published* prediction.  A deferred prediction may still be
        // pending; it was never delivered to this load, so record the actual
        // value without verifying (and drop the pending marker so a late
        // publish cannot resurrect it).
        q.pending = false;
        q.value = actualVal;
        q.avail = true;
    }

    // ---- Deferred prediction wakeup (Section III-C) ----
    //
    // Only publish the wake when this base load itself verified correctly.
    // If the base value-mispredicted, the whole younger window is about to be
    // squashed, so a wake must not be generated (it would be a phantom event:
    // the caller never delivers it, and the dependent's VP must stay clear).
    if (q.wakeSeqNum != 0) {
        auto dit = std::find_if(specGVQ.begin(), specGVQ.end(),
                                [&q](const EgDiffSpecEntry &x) {
                                    return x.seqNum == q.wakeSeqNum;
                                });
        if (dit != specGVQ.end()) {
            const size_t depIdx = std::distance(specGVQ.begin(), dit);
            EgDiffSpecEntry &dep = *dit;

            const bool stillValid =
                !dep.avail && !dep.vp && !dep.pending &&
                (depIdx - curIdx) == dep.distance;

            if (!stillValid) {
                // Section III-C: dependent completed before the base (or its
                // relative distance shifted) -> ignore the obsolete binding.
                res.wakeIgnored = true;
                res.wakeIgnoredSeq = dep.seqNum;
            } else if (res.ok && !predictionSuppressed()) {
                const uint64_t predVal = actualVal + dep.diff;
                // The value exists but is not visible yet.  `vp` is set by
                // publishDeferred() when the microarchitecture actually
                // writes it into the destination register.
                dep.value = predVal;
                dep.pending = true;
                res.wakeTarget = &dep;
                res.wake.valid = true;
                res.wake.value = predVal;
                res.wake.distance = dep.distance;
                res.wake.baseVal = actualVal;
                res.wake.basePc = q.pc;
            }
        }
        // Consume the binding regardless.
        q.wakeSeqNum = 0;
    }

    return res;
}

void
EgDiffPredictor::publishDeferred(uint64_t seqNum)
{
    auto it = std::find_if(specGVQ.begin(), specGVQ.end(),
                           [seqNum](const EgDiffSpecEntry &x) {
                               return x.seqNum == seqNum;
                           });
    if (it == specGVQ.end())
        return;

    // Only a prediction that (a) still exists, (b) has not already completed,
    // and (c) is still pending may become visible.  If the load completed
    // first it already recorded its actual value and must not be overwritten.
    if (it->pending && !it->avail && !it->vp) {
        it->pending = false;
        it->vp = true;
    }
}

unsigned
EgDiffPredictor::squashFrom(uint64_t squashNum)
{
    // Drop every speculative entry younger than the squash bound (these are
    // the entries with seqNum > squashNum).  The NVQ and the Prediction
    // Table are non-speculative and are deliberately left intact.
    while (!specGVQ.empty() && specGVQ.back().seqNum > squashNum)
        specGVQ.pop_back();

    // Clear any wake binding whose target was squashed.
    unsigned cleared = 0;
    for (auto &e : specGVQ) {
        if (e.wakeSeqNum > squashNum) {
            e.wakeSeqNum = 0;
            ++cleared;
        }
    }

    // A value misprediction requested a window; start it now, after the
    // squash, so only correct-path instructions consume the 1024 slots.
    if (suppressPending) {
        suppressUntilCount = instructionCount + LAST_MISP_WINDOW;
        suppressPending = false;
    }
    return cleared;
}

void
EgDiffPredictor::retireFront(uint64_t seqNum)
{
    while (!specGVQ.empty() && specGVQ.front().seqNum <= seqNum)
        specGVQ.pop_front();
}

void
EgDiffPredictor::reset()
{
    std::fill(table.begin(), table.end(), EgDiffTableEntry{});
    nvq.fill(0);
    specGVQ.clear();
    tickCounter = 0;
    suppressUntilCount = 0;
    suppressPending = false;
    instructionCount = 0;
}

} // namespace lvp
} // namespace gem5
