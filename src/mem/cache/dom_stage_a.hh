#ifndef __MEM_CACHE_DOM_STAGE_A_HH__
#define __MEM_CACHE_DOM_STAGE_A_HH__

#include <cstdint>
#include <fstream>
#include <unordered_map>

#include "base/types.hh"
#include "mem/packet.hh"

namespace gem5
{

class MSHR;
class Request;

/**
 * Stage A preliminary trace probe.
 *
 * This is instrumentation only: it does not delay, allocate, or otherwise
 * change timing.  Enabled by GEM5_DOM_STAGE_A_TRACE=<path>.
 *
 * Text trace v0 (a lower bound on the DoM-delayed set):
 *
 *   M <fill_tick> <pc> <addr> <size> <value> <latency>
 *   S <commit_tick> <pc> <addr> <size> <value>
 *
 * M is an L1-D load miss and is logged when the fill returns, so the value
 * is the architectural value at that point.  S is a committed store (O3
 * drains stores at commit).  Speculative hits on still-speculative lines
 * are added in the provenance layer (A2.2); until then this misses part of
 * the DoM-delayed set.
 */
class DomStageAProbe
{
  public:
    static DomStageAProbe &get();

    bool enabled() const { return _enabled; }

    /** Called for every CPU-side request after the tag access. */
    void request(PacketPtr pkt, bool hit);

    /** Called on a fill response; logs each waiting target. */
    void fill(PacketPtr pkt, MSHR *mshr, unsigned blk_size);

  private:
    DomStageAProbe();
    ~DomStageAProbe();

    struct Pending
    {
        Tick tick;
        Addr pc;
        Addr addr;
        unsigned size;
    };

    void log_store(PacketPtr pkt);
    void flush_if_needed();

    std::ofstream _out;
    bool _enabled;
    unsigned _lines;
    std::unordered_map<const Request *, Pending> _pending;
};

} // namespace gem5

#endif // __MEM_CACHE_DOM_STAGE_A_HH__
