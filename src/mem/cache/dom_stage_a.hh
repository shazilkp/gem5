#ifndef __MEM_CACHE_DOM_STAGE_A_HH__
#define __MEM_CACHE_DOM_STAGE_A_HH__

#include <cstdint>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include "base/types.hh"
#include "mem/packet.hh"

namespace gem5
{

class MSHR;
class Request;
class CacheBlk;

/**
 * Stage A trace probe v1 (DoM delayed set + safe point + cost inputs).
 *
 * Enabled by GEM5_DOM_STAGE_A_TRACE=<path>.  Read-only: it never changes
 * cache state or timing.
 *
 * Trace:
 *   L <seq> <tid> <pc> <addr> <size> <value> <issue> <latency> <kind>
 *     <fillSeq> <commitSeq> <fillLatency>
 *   V <seq> <safe_tick>
 *   C <seq> <commit_tick>
 *   X <seq> <squash_tick>
 *   S <tick> <pc> <addr> <size> <value>
 *
 * L kind 0 = L1 miss; 1 = hit on a line installed by a still-speculative
 * fill (a DoM miss).  V is the tick the load became the ROB head (v1 safe
 * point).  C/X mark commit/squash; only C loads are architectural.
 */
class DomStageAProbe
{
  public:
    static DomStageAProbe &get();

    bool enabled() const { return _enabled; }

    /** Called for every CPU-side request after the tag access. */
    void request(PacketPtr pkt, bool hit, CacheBlk *blk, unsigned blk_size);

    /** Called on a fill response; logs each waiting target. */
    void fill(PacketPtr pkt, MSHR *mshr, unsigned blk_size);

    /** The ROB head advanced to this sequence number. */
    void head(uint64_t seq, Tick tick);

    /** This sequence number committed / was squashed. */
    void commit(uint64_t seq, Tick tick);
    void squash(uint64_t seq, Tick tick);

  private:
    DomStageAProbe();
    ~DomStageAProbe();

    struct Pending
    {
        Tick tick;
        Addr pc;
        Addr addr;
        unsigned size;
        uint64_t seq;
        uint64_t commit_seq;
        uint64_t tid;
    };

    void log_load(uint64_t seq, uint64_t tid, Addr pc, Addr addr,
                  unsigned size, uint64_t value, Tick issue, Tick latency,
                  unsigned kind, uint64_t fill_seq, uint64_t commit_seq,
                  uint64_t fill_latency);
    void log_store(PacketPtr pkt);
    void flush_if_needed();

    std::ofstream _out;
    bool _enabled;
    unsigned _lines;
    std::unordered_map<const Request *, Pending> _pending;
    std::unordered_set<uint64_t> _pending_seqs;
    std::unordered_set<uint64_t> _delayed;
};

} // namespace gem5

#endif // __MEM_CACHE_DOM_STAGE_A_HH__
