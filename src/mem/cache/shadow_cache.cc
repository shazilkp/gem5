/*
 * SpaceSpec shadow cache (Layer 1).  See shadow_cache.hh.
 */

#include "mem/cache/shadow_cache.hh"

#include <cstdlib>
#include <cstring>

#include "base/logging.hh"
#include "debug/SpaceSpec.hh"
#include "mem/cache/base.hh"

namespace gem5
{

namespace
{

/** Read a boolean environment variable, defaulting to true when unset. */
bool
envFlag(const char *name)
{
    const char *v = std::getenv(name);
    if (!v) {
        return true;
    }
    return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0 ||
             std::strcmp(v, "no") == 0);
}

} // namespace

ShadowCache::ShadowCache(const Params &p)
    : SimObject(p),
      buffer_(p.num_entries, p.line_size),
      stats(this)
{
    fatal_if(p.line_size == 0 || (p.line_size & (p.line_size - 1)) != 0,
             "ShadowCache: line_size must be a non-zero power of two, got %d",
             p.line_size);
    fatal_if(p.num_entries == 0,
             "ShadowCache: num_entries must be > 0");

    // Bisection gates. Read once, at construction, so a run cannot change
    // behaviour halfway through -- a mechanism that behaves differently in the
    // second half of a simulation than in the first is not diagnosable.
    intercept_ = envFlag("SPACESPEC_INTERCEPT");
    promote_ = envFlag("SPACESPEC_PROMOTE");
    divert_ = envFlag("SPACESPEC_DIVERT");
    store_hooks_ = envFlag("SPACESPEC_STORES");

    if (!intercept_ || !promote_ || !divert_ || !store_hooks_) {
        warn("%s: bisection gate active -- intercept=%d promote=%d divert=%d "
             "stores=%d. This is NOT a working configuration; it exists to "
             "attribute a failure.", name(), (int)intercept_, (int)promote_,
             (int)divert_, (int)store_hooks_);
    }
}

ShadowCache::ShadowStats::ShadowStats(ShadowCache *sc)
    : statistics::Group(sc),
      ADD_STAT(captures, statistics::units::Count::get(),
               "Fills diverted into the shadow cache"),
      ADD_STAT(overflows, statistics::units::Count::get(),
               "Captures refused because the shadow was full"),
      ADD_STAT(refreshes, statistics::units::Count::get(),
               "Captures of a line already held (contents refreshed)"),
      ADD_STAT(lookupHits, statistics::units::Count::get(),
               "Loads served from the shadow cache"),
      ADD_STAT(lookupMisses, statistics::units::Count::get(),
               "Loads that found nothing in the shadow cache"),
      ADD_STAT(promotions, statistics::units::Count::get(),
               "Lines installed into the cache at commit"),
      ADD_STAT(storeDrops, statistics::units::Count::get(),
               "Lines dropped because a store reached them"),
      ADD_STAT(squashes, statistics::units::Count::get(),
               "Lines discarded because their load was squashed"),
      ADD_STAT(peakOccupancy, statistics::units::Count::get(),
               "Peak simultaneous shadow entries")
{
}

// ------------------------------------------------------------------------- //
//  Capture                                                                    //
// ------------------------------------------------------------------------- //

bool
ShadowCache::capture(std::uint64_t ownerSeq, std::uint64_t addr,
                     const std::uint8_t *data, std::size_t len)
{
    const std::size_t line = buffer_.lineSize();
    // The cache hands us a whole line. Anything else means the interception
    // point is wrong, and silently storing a partial line would produce
    // plausible-looking garbage on a later lookup at a different offset.
    fatal_if(len != line,
             "ShadowCache::capture: %d bytes for a %d-byte line", len, line);

    const CaptureResult r = buffer_.capture(ownerSeq, addr, data);

    const auto &s = buffer_.stats();
    stats.captures = s.captures;
    stats.overflows = s.overflows;
    stats.refreshes = s.refreshes;
    stats.peakOccupancy = s.peakOccupancy;

    if (r == CaptureResult::Overflow) {
        DPRINTF(SpaceSpec,
                "%s: shadow full, fill for [sn:%llu] addr %#x dropped\n",
                name(), ownerSeq, addr);
        return false;
    }

    DPRINTF(SpaceSpec, "%s: captured addr %#x for [sn:%llu] (occupancy %d/%d)\n",
            name(), addr, ownerSeq, (int)buffer_.size(),
            (int)buffer_.capacity());
    return true;
}

bool
ShadowCache::lookup(std::uint64_t addr, unsigned offset,
                    std::uint8_t *dst, std::size_t len)
{
    const bool hit = buffer_.lookup(addr, offset, dst, len);
    const auto &s = buffer_.stats();
    stats.lookupHits = s.lookupHits;
    stats.lookupMisses = s.lookups - s.lookupHits;
    if (hit) {
        DPRINTF(SpaceSpec, "%s: served addr %#x from shadow\n", name(), addr);
    }
    return hit;
}

// ------------------------------------------------------------------------- //
//  Release                                                                    //
// ------------------------------------------------------------------------- //

bool
ShadowCache::promote(std::uint64_t ownerSeq)
{
    if (!promote_) {
        return false;
    }

    std::uint64_t addr = 0;
    std::vector<std::uint8_t> data;
    if (!buffer_.promote(ownerSeq, addr, data)) {
        // Benign: not speculative, the capture was refused because the buffer
        // was full, or a store dropped it. Nothing was consumed.
        DPRINTF(SpaceSpec,
                "%s: nothing to promote for [sn:%llu]\n", name(), ownerSeq);
        return false;
    }

    ++stats.promotions;
    DPRINTF(SpaceSpec,
            "%s: promoting addr %#x for [sn:%llu]\n", name(), addr, ownerSeq);

    if (cache_) {
        cache_->promoteFromShadow(addr, data.data());
    } else {
        warn_once("%s: no cache registered; promoted line for [sn:%llu] "
                  "discarded", name(), ownerSeq);
    }
    return true;
}

bool
ShadowCache::discard(std::uint64_t ownerSeq)
{
    std::uint64_t addr = 0;
    std::vector<std::uint8_t> data;
    if (!buffer_.promote(ownerSeq, addr, data)) {
        return false;
    }
    ++stats.squashes;
    DPRINTF(SpaceSpec,
            "%s: discarded addr %#x for [sn:%llu] (load squashed)\n",
            name(), addr, ownerSeq);
    return true;
}

bool
ShadowCache::dropLine(std::uint64_t addr)
{
    if (!buffer_.dropLine(addr)) {
        return false;
    }
    ++stats.storeDrops;
    DPRINTF(SpaceSpec, "%s: dropped addr %#x (a store reached the line)\n",
            name(), addr);
    return true;
}

std::size_t
ShadowCache::discardFrom(std::uint64_t squashSeq)
{
    const std::size_t n = buffer_.discardFrom(squashSeq);
    stats.squashes = buffer_.stats().discards;
    if (n) {
        DPRINTF(SpaceSpec,
                "%s: discarded %d entries squashed at [sn:%llu]\n",
                name(), (int)n, squashSeq);
    }
    return n;
}

std::size_t
ShadowCache::retireThrough(std::uint64_t watermark)
{
    return buffer_.retireThrough(watermark);
}

} // namespace gem5