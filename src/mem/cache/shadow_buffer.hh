/*
 * SpaceSpec shadow buffer (Layer 0).
 *
 * A small, self-contained structure holding cache lines that were fetched
 * speculatively and must not become visible in a real cache until the owning
 * instruction commits.
 *
 * Deliberately NOT a SimObject and deliberately free of any cache, pipeline
 * or statistics dependency.  Layer 0 exists so that ownership and lifetime --
 * the two things that actually went wrong in the first implementation -- can
 * be specified and tested in isolation, with no simulator involved.  Wiring
 * this into gem5 happens in Layer 1, once this is known to be correct.
 *
 * ---------------------------------------------------------------------------
 * Ownership
 * ---------------------------------------------------------------------------
 * Several loads can touch the same line.  Exactly one of them "owns" the
 * entry, and that owner is the one whose commit installs the line.
 *
 * The rule is: the owner is the OLDEST load that has been captured and not
 * yet retired.  Not the newest, and not "the one that happens to be at the
 * commit head".
 *
 * Why oldest, which was the original mistake:
 *
 *   The first implementation inserted each line under the newest load that
 *   fetched it, then installed it by looking up the *committing* load's
 *   sequence number.  Those two keys were never the same, so promote() never
 *   matched, so the line never reached the cache.  Every access re-fetched and
 *   re-captured it, without bound.  That is the livelock that ended the first
 *   implementation, and it presented as a hang rather than as a wrong answer,
 *   which is why it took a bisection to find.
 *
 *   Taking the oldest capturing load as owner, and refreshing contents without
 *   re-keying, removes that mismatch: the owner is the load whose commit will
 *   release the line, by construction.
 *
 * Two further properties keep a missed release from mattering:
 *
 *   - A refresh keeps the ORIGINAL owner even when a younger load re-fetches
 *     the line.  Re-keying to the newer load was the other half of the same
 *     bug: the entry would then be released only by that newer load, and if it
 *     were squashed in the meantime the entry was stranded.
 *
 *   - A lookup that misses is not fatal, because retireThrough() sweeps by
 *     watermark regardless of which instruction asked.  So if the owner is
 *     squashed, or its capture was refused because the buffer was full, or the
 *     store dropped the line, the entry is reclaimed anyway once the watermark
 *     passes.  promote() is the fast path that installs the line; the sweep is
 *     what makes a miss harmless rather than permanent.
 *
 * ---------------------------------------------------------------------------
 * Lifetime
 * ---------------------------------------------------------------------------
 * Entries are added by one event and removed by four, all driven by the caller:
 *
 *   capture()       a line is added, tagged with the seqNum of its owner
 *
 *   promote()       the owner's load committed; hand the line out and install
 *                   it in the real cache
 *   discardFrom()   the owner was squashed; throw the line away
 *   dropLine()      a store reached the line, making the held copy stale
 *   retireThrough() a watermark sweep, run at retirement
 *
 * The caller supplies the watermark.  This keeps the structure free of any
 * dependency on a ROB, a CPU or a thread model, and it means the buffer cannot
 * disagree with the pipeline about what has retired: the pipeline is the single
 * source of truth, and simply reports it.
 *
 * The sweep is what makes "no path can retain an entry forever" true, and it
 * is not redundant with promote().  It exists because of a late fill:
 *
 *   A load can miss, then be squashed by a branch before its fill returns.  The
 *   fill arrives anyway -- a cache's MSHRs service squashed targets, because
 *   they have no notion of squashed instructions -- so capture() runs against a
 *   sequence number that will never commit, and that the already-executed
 *   discard could not have covered.  Such an entry is stranded: no promote()
 *   will ever match a dead owner, and a later access to the same line refreshes
 *   it while keeping that dead owner.  The line then never reaches the real
 *   cache and every access to it re-fetches, without bound.  That is the
 *   livelock which ended the first implementation, and it presented as a hang
 *   rather than as a wrong answer.
 *
 *   The sweep reclaims it, because the watermark is monotonic and never
 *   reuses sequence numbers, so it is guaranteed to pass the dead owner
 *   eventually.  Recovery is therefore deferred rather than immediate: until
 *   the watermark arrives, the line is not installed and will re-miss.  That
 *   is a bounded cost, and it is the difference between bounded and unbounded.
 *
 * Note the directions differ and the two cannot be substituted:
 *
 *   retireThrough(w)  removes the OLDEST entries, owner <  w   (they retired)
 *   discardFrom(s)    removes the NEWEST entries, owner >= s (they squashed)
 *
 * Passing a squash bound to retireThrough() discards the lines of loads that
 * were about to commit.  Nothing crashes; the cache simply stops working.
 *
 * ---------------------------------------------------------------------------
 * Ordering
 * ---------------------------------------------------------------------------
 * Entries are kept sorted by owner sequence number, ascending, and capture()
 * inserts rather than appends.  Loads do not fill in program order -- an older
 * load can miss and fill after a younger one already has -- so arrival order is
 * not sequence order, and both retireThrough() and discardFrom() are single
 * walks from one end of the queue.  The sort is load-bearing, and
 * checkInvariants() verifies it.
 *
 * Full-buffer policy
 * ---------------------------------------------------------------------------
 * SafeSpec forbids evicting one speculative line to admit another: that
 * turns the buffer into a contention side channel.  It stalls instead.
 *
 * This buffer reports overflow to the caller and refuses the capture.  It
 * does not evict, and it does not stall on its own, because neither belongs
 * in a data structure that knows nothing about time.  The caller decides
 * what to do -- stall, drop the fill, or resize -- and can distinguish the
 * two cases:
 *
 *   capture() == Accepted   the line is in the buffer
 *   capture() == Overflow   the buffer is full; the line was NOT stored and
 *                           the caller must not assume it can be promoted
 *
 * Overflow is counted, so sizing can be justified against a measurement
 * rather than against a figure copied from a paper.
 */

#ifndef __MEM_CACHE_SHADOW_BUFFER_HH__
#define __MEM_CACHE_SHADOW_BUFFER_HH__

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <vector>

namespace gem5
{

/**
 * One captured line.
 */
struct ShadowEntry
{
    /** Block-aligned address of the line. */
    std::uint64_t addr = 0;

    /** Owner: the oldest captured, not-yet-retired load using this line. */
    std::uint64_t ownerSeq = 0;

    /** Line contents. */
    std::vector<std::uint8_t> data;

    bool valid = false;
};

/** Outcome of attempting to capture a line. */
enum class CaptureResult
{
    /** Stored in the buffer. */
    Accepted,
    /**
     * The buffer was full.  The line was NOT stored, so there is nothing to
     * promote and the caller must treat the line as absent.
     */
    Overflow
};

/** What release() did, for callers that want to attribute it. */
struct CaptureCounters
{
    std::size_t captures = 0;
    std::size_t refreshes = 0;
    std::size_t overflows = 0;
    std::size_t promotions = 0;
    std::size_t drops = 0;
    std::size_t retires = 0;
    std::size_t lookups = 0;
    std::size_t lookupHits = 0;

    /** High-water mark of occupancy, for justifying capacity. */
    std::size_t peakOccupancy = 0;
};

/**
 * Fixed-capacity, FIFO-ordered buffer of speculatively captured lines.
 *
 * Ordering is by owner sequence number, ascending.  The front of the queue is
 * therefore always the oldest live entry, which is what makes release a
 * single forward walk.
 */
class ShadowBuffer
{
  public:
    /**
     * @param numEntries  capacity, in lines
     * @param lineSize    bytes per line
     */
    ShadowBuffer(std::size_t numEntries, std::size_t lineSize)
        : capacity_(numEntries), lineSize_(lineSize)
    {
    }

    /**
     * Capture a line on behalf of a load.
     *
     * If the line is already held, the existing entry is refreshed and keeps
     * its original (older) owner.  Re-keying to the newer load was the other
     * half of the original livelock: the entry would then be released only
     * when the newer load retired, and if that load was squashed in the
     * meantime the entry was stranded.
     *
     * A new entry is inserted in owner-sequence order rather than appended.
     * Loads do not capture in sequence order, and retireThrough() relies on
     * the queue being sorted, so this is load-bearing rather than cosmetic.
     *
     * @param ownerSeq  sequence number of the capturing load
     * @param addr      block-aligned address
     * @param src       lineSize bytes
     * @return Accepted if held afterwards (whether newly captured or
     *         already present), Overflow if the buffer was full.
     */
    CaptureResult
    capture(std::uint64_t ownerSeq, std::uint64_t addr,
            const std::uint8_t *src)
    {
        ++stats_.captures;

        // Already held: refresh the contents, keep the older owner.
        for (auto &e : entries_) {
            if (e.valid && e.addr == addr) {
                std::memcpy(e.data.data(), src, lineSize_);
                ++stats_.refreshes;
                return CaptureResult::Accepted;
            }
        }

        if (entries_.size() >= capacity_) {
            ++stats_.overflows;
            return CaptureResult::Overflow;
        }

        ShadowEntry e;
        e.addr = addr;
        e.ownerSeq = ownerSeq;
        e.data.assign(src, src + lineSize_);
        e.valid = true;

        // Insert in owner order.  retireThrough() releases from the front and
        // stops at the first live entry, which is only correct if the queue
        // is sorted by owner sequence number.  Loads do not capture in
        // sequence order -- an older load can miss later, after a younger one
        // has already captured the same or another line -- so the position
        // has to be found rather than appended to.
        auto pos = entries_.begin();
        while (pos != entries_.end() && pos->ownerSeq < ownerSeq)
            ++pos;
        entries_.insert(pos, std::move(e));
        stats_.noteOccupancy(entries_.size());
        return CaptureResult::Accepted;
    }

    /**
     * Retrieve a line.
     *
     * Only for accesses that are themselves part of the same speculation
     * window.  This is an optimisation -- a miss here just means the caller
     * goes to memory -- and it must not be used to satisfy an access that
     * has already committed, because the buffer may hold an older copy.
     */
    bool
    lookup(std::uint64_t addr, unsigned offset,
           std::uint8_t *dst, std::size_t len) const
    {
        ++stats_.lookups;
        for (const auto &e : entries_) {
            if (e.valid && e.addr == addr) {
                if (offset + len > lineSize_)
                    return false;
                std::memcpy(dst, e.data.data() + offset, len);
                ++stats_.lookupHits;
                return true;
            }
        }
        return false;
    }

    /** True if a line is currently held. */
    bool
    holds(std::uint64_t addr) const
    {
        for (const auto &e : entries_) {
            if (e.valid && e.addr == addr)
                return true;
        }
        return false;
    }

    /**
     * Promote the line owned by a load, removing it from the buffer.
     *
     * The address is returned alongside the data because the caller needs it
     * to install the line somewhere.  Asking for it separately would mean a
     * second lookup, and two lookups can disagree if the buffer is mutated
     * between them.
     *
     * @param ownerSeq  the owning load's sequence number
     * @param addrOut   receives the block-aligned address
     * @param out       receives a copy of the line contents
     * @return true if an entry was owned by ownerSeq and has been released.
     */
    bool
    promote(std::uint64_t ownerSeq, std::uint64_t &addrOut,
            std::vector<std::uint8_t> &out)
    {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->valid && it->ownerSeq == ownerSeq) {
                addrOut = it->addr;
                out = it->data;
                entries_.erase(it);
                ++stats_.promotions;
                return true;
            }
        }
        return false;
    }

    /**
     * Drop a line outright, e.g. because a store reached it.
     *
     * @return true if an entry was dropped.
     */
    bool
    dropLine(std::uint64_t addr)
    {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->valid && it->addr == addr) {
                entries_.erase(it);
                ++stats_.drops;
                return true;
            }
        }
        return false;
    }

    /**
     * Release every entry owned by a load that has already retired.
     *
     * @param watermark  every load with a sequence number strictly below this
     *                   has committed or been squashed.  Entries owned by such
     *                   a load are dead and are removed.
     *
     * Entries are released regardless of *why* their owner retired.  That is
     * what makes this robust: a squashed owner releases its entry just as a
     * committed one does, so a squash can never strand an entry.
     *
     * The walk stops at the first entry still owned by a live load, because
     * the queue is ordered by owner sequence number.  An entry behind a live
     * one is always newer, hence also live.
     */
    std::size_t
    retireThrough(std::uint64_t watermark)
    {
        std::size_t released = 0;
        while (!entries_.empty() && entries_.front().ownerSeq < watermark) {
            entries_.pop_front();
            ++released;
            ++stats_.retires;
        }
        return released;
    }

    /**
     * Discard every entry owned by a load at or above a sequence number.
     *
     * This is the squash path.  A squash removes the squashing instruction
     * and everything younger from the pipeline, so the lines they fetched must
     * be discarded: they were speculative and are now wrong.  Note the
     * direction is the *opposite* of retireThrough() -- that one releases the
     * oldest entries that have retired, this one throws away the newest that
     * have been squashed -- and getting the two backwards is a subtle way to
     * silently discard the lines of loads that are about to commit.
     *
     * @param squashSeq  loads at or above this sequence number are squashed
     * @return how many entries were discarded.
     */
    std::size_t
    discardFrom(std::uint64_t squashSeq)
    {
        std::size_t removed = 0;
        while (!entries_.empty() && entries_.back().ownerSeq >= squashSeq) {
            entries_.pop_back();
            ++removed;
            ++stats_.discards;
        }
        return removed;
    }

    /** Number of lines currently held. */
    std::size_t
    size() const
    {
        return entries_.size();
    }

    /** Maximum number of lines this buffer can hold. */
    std::size_t
    capacity() const
    {
        return capacity_;
    }

    /** Bytes per line. */
    std::size_t
    lineSize() const
    {
        return lineSize_;
    }

    bool
    empty() const
    {
        return entries_.empty();
    }

    /** Sequence number of the oldest live entry, if any. */
    std::optional<std::uint64_t>
    oldestOwner() const
    {
        if (entries_.empty())
            return std::nullopt;
        return entries_.front().ownerSeq;
    }

    /** Sequence number of the newest live entry, if any. */
    std::optional<std::uint64_t>
    newestOwner() const
    {
        if (entries_.empty())
            return std::nullopt;
        return entries_.back().ownerSeq;
    }

    /**
     * Invariant check, for tests and for debug builds.
     *
     * Verifies that the queue is ordered by ascending owner sequence number
     * and that every entry is valid with correctly sized data.  O(n), so this
     * is a test/debug facility rather than something for the hot path.
     */
    bool
    checkInvariants() const
    {
        std::uint64_t prev = 0;
        bool first = true;
        for (const auto &e : entries_) {
            if (!e.valid)
                return false;
            if (e.data.size() != lineSize_)
                return false;
            if (!first && e.ownerSeq < prev)
                return false;
            prev = e.ownerSeq;
            first = false;
        }
        return true;
    }

    /** Counters, for sizing decisions and for tests. */
    struct Stats
    {
        std::size_t captures = 0;
        std::size_t refreshes = 0;
        std::size_t overflows = 0;
        std::size_t promotions = 0;
        std::size_t drops = 0;
        std::size_t retires = 0;
        std::size_t discards = 0;
        std::size_t lookups = 0;
        std::size_t lookupHits = 0;

        /** High-water mark of occupancy, for justifying capacity. */
        std::size_t peakOccupancy = 0;

        void
        noteOccupancy(std::size_t n)
        {
            if (n > peakOccupancy)
                peakOccupancy = n;
        }
    };

    const Stats &
    stats() const
    {
        return stats_;
    }

  private:
    std::size_t capacity_;
    std::size_t lineSize_;

    /** Ordered by ownerSeq ascending; front is the oldest live entry. */
    std::deque<ShadowEntry> entries_;

    /**
     * Mutable so that const accessors can still count.  Counting a lookup is
     * not a semantic change to the buffer's contents, and keeping the
     * counters here avoids a second parallel accounting path that could
     * disagree with this one.
     */
    mutable Stats stats_;
};

} // namespace gem5

#endif // __MEM_CACHE_SHADOW_BUFFER_HH__