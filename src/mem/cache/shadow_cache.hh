/*
 * SpaceSpec shadow cache (Layer 1).
 *
 * A thin SimObject shell around ShadowBuffer.  Layer 0 established and unit
 * tested the buffer itself -- ownership, lifetime, overflow policy -- with no
 * gem5 dependency at all.  This file adds only what wiring requires: a
 * SimObject so it can be referenced from configuration, a link to the cache it
 * guards, and the promotion step that installs a retired line into that cache.
 *
 * The split is deliberate.  Everything that was hard to get right lives in
 * Layer 0 and is covered by unit tests; what remains here is glue, and glue
 * should be as small as possible.
 *
 * Diverting a fill without installing it is the whole mechanism.  That happens
 * in BaseCache::recvTimingResp(), which forces allocate=false so the line goes
 * to the existing tempBlock scratch path and never becomes resident.  The line
 * is captured here instead.  Promotion, below, is the only way a line ever
 * reaches the real cache.
 */

#ifndef __MEM_CACHE_SHADOW_CACHE_HH__
#define __MEM_CACHE_SHADOW_CACHE_HH__

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/shadow_buffer.hh"
#include "params/ShadowCache.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class BaseCache;

/**
 * Shadow state for one L1 data cache.
 *
 * Shared between the guarded cache and the CPU: the cache fills this and
 * serves lookups from it, while commit promotes out of it and the ROB
 * discards from it.
 */
class ShadowCache : public SimObject
{
  public:
    PARAMS(ShadowCache);
    explicit ShadowCache(const Params &p);

    /**
     * Register the cache whose fills are being shadowed.
     *
     * Called from BaseCache::init().  Doing it here rather than through
     * configuration means the two cannot disagree about which cache is
     * guarded, and means Commit can find the cache again without the CPU
     * holding a reference to it.
     */
    void setCache(BaseCache *c) { cache_ = c; }
    BaseCache *cache() const { return cache_; }

    // ---------------------------------------------------------------- //
    //  Capture (called by the cache on a speculative fill)              //
    // ---------------------------------------------------------------- //

    /**
     * Capture a speculatively fetched line.
     *
     * @return true if the line is held afterwards.  False means the buffer
     *         was full; the caller must then treat the line as absent, and
     *         must not install it.
     */
    bool capture(std::uint64_t ownerSeq, std::uint64_t addr,
                 const std::uint8_t *data, std::size_t len);

    /**
     * Serve a line that is currently held.
     *
     * Purely an optimisation: a miss just means the caller goes to memory.
     * Must not be used to satisfy an access that has already committed,
     * because a held copy may be older than the real cache.
     */
    bool lookup(std::uint64_t addr, unsigned offset,
                std::uint8_t *dst, std::size_t len);

    // ---------------------------------------------------------------- //
    //  Release (called by commit and the ROB)                           //
    // ---------------------------------------------------------------- //

    /**
     * Promote the line owned by a load that has just committed, installing it
     * into the guarded cache.
     *
     * @return true if an entry was owned by ownerSeq.  False is benign and
     *         has three causes: the load was not speculative, its capture was
     *         refused because the buffer was full, or a store invalidated it.
     */
    bool promote(std::uint64_t ownerSeq);

    /**
     * Discard the line owned by a squashed load.  The line never becomes
     * visible anywhere, which is the entire point.
     *
     * @return true if an entry was discarded.
     */
    bool discard(std::uint64_t ownerSeq);

    /**
     * Drop a line outright, because a store reached it.
     *
     * The held copy is by definition older than the store, so promoting it
     * afterwards would install stale data over the store.  This is the one
     * rule that makes deferred promotion safe in the presence of stores.
     *
     * @return true if an entry was dropped.
     */
    bool dropLine(std::uint64_t addr);

    /**
     * Discard every entry owned by a load that has been squashed.
     *
     * Called from ROB::squash(), which covers branch mispredictions, LVP
     * mismatches, memory-order violations, traps and TC squashes in one
     * place.
     *
     * @param squashSeq  loads at or above this sequence number are squashed.
     *                   ROB::squash() receives an already-exclusive bound --
     *                   Commit decrements it when the squashing instruction
     *                   itself is included, as happens for an LVP mismatch --
     *                   so the violating load's own entry is covered here.
     * @return how many entries were discarded.
     */
    std::size_t discardFrom(std::uint64_t squashSeq);

    /**
     * Release every entry whose owner has passed the retirement watermark.
     *
     * The complement of discardFrom(): this drops the *oldest* entries, which
     * have committed, whereas a squash drops the *newest*.  Substituting one
     * for the other silently loses the lines of loads that are about to
     * commit.
     *
     * @param watermark  loads below this sequence number have committed.
     */
    std::size_t retireThrough(std::uint64_t watermark);

    // ---------------------------------------------------------------- //
    //  Bisection gates                                                  //
    // ---------------------------------------------------------------- //

    /**
     * Should fills be diverted into the shadow at all?
     *
     * Runtime-gated rather than compile-time so a failure can be bisected
     * without a rebuild, which is the only practical way to attribute a
     * symptom that surfaces thousands of instructions from its cause.  With
     * interception off the cache behaves exactly as stock gem5: nothing is
     * captured, so promotion never fires either.
     *
     * SPACESPEC_INTERCEPT=0 disables it.
     */
    bool interceptEnabled() const { return intercept_; }

    /**
     * Should committed lines be installed back into the cache?
     *
     * Separate from interceptEnabled() because the two halves fail
     * differently: interception decides whether speculative lines can exist at
     * all, promotion decides whether the cache still works.  Turning only this
     * off is the known-degenerate case where nothing is ever promoted, so
     * every access re-fetches; it is useful for confirming a symptom comes
     * from the install path rather than the divert path.
     *
     * SPACESPEC_PROMOTE=0 disables it.
     */
    bool promotionEnabled() const { return promote_; }

    /**
     * Should a captured fill actually be kept OUT of the cache?
     *
     * The divert is the mechanism: without it, a speculative line is installed
     * speculatively and the whole guarantee is void.  This gate exists only to
     * separate "the divert corrupts something" from "the bookkeeping corrupts
     * something", by capturing into the shadow while letting the fill install
     * normally.  In that mode promotion is a no-op, because the line is already
     * resident.
     *
     * SPACESPEC_DIVERT=0 disables the divert.
     */
    bool divertEnabled() const { return divert_; }

    /**
     * Should stores invalidate shadow and promoted lines?
     *
     * These two hooks are what make deferred promotion safe in the presence of
     * stores: a store makes a held copy stale, and a store to a promoted line
     * would make it dirty and unannounced at the same time.  Turning them off
     * is expected to be incorrect; it is here to attribute a failure.
     *
     * SPACESPEC_STORES=0 disables them.
     */
    bool storeHooksEnabled() const { return store_hooks_; }

    /** True if a line is currently held. */
    bool holds(std::uint64_t addr) const { return buffer_.holds(addr); }

    /** Number of lines currently held. */
    std::size_t size() const { return buffer_.size(); }

    /** The underlying buffer, for diagnostics and tests. */
    const ShadowBuffer &buffer() const { return buffer_; }

  private:
    ShadowBuffer buffer_;

    /** The guarded cache, set by BaseCache::init(). */
    BaseCache *cache_ = nullptr;

    /** Bisection gate: divert speculative fills into the shadow. */
    bool intercept_ = true;

    /** Bisection gate: install committed lines back into the cache. */
    bool promote_ = true;

    /** Bisection gate: keep captured fills out of the cache. */
    bool divert_ = true;

    /** Bisection gate: let stores invalidate shadow and promoted lines. */
    bool store_hooks_ = true;

    struct ShadowStats : public statistics::Group
    {
        explicit ShadowStats(ShadowCache *sc);

        /** Fills diverted into the shadow. */
        statistics::Scalar captures;
        /** Captures refused because the buffer was full. */
        statistics::Scalar overflows;
        /** Captures of a line already held (contents refreshed). */
        statistics::Scalar refreshes;
        /** Loads served from the shadow without reaching the cache. */
        statistics::Scalar lookupHits;
        /** Loads that found nothing in the shadow. */
        statistics::Scalar lookupMisses;
        /** Lines installed into the cache at commit. */
        statistics::Scalar promotions;
        /** Lines dropped because a store reached them. */
        statistics::Scalar storeDrops;
        /** Lines discarded because their load was squashed. */
        statistics::Scalar squashes;
        /** High-water mark of occupancy. */
        statistics::Scalar peakOccupancy;
    } stats;
};

} // namespace gem5

#endif // __MEM_CACHE_SHADOW_CACHE_HH__