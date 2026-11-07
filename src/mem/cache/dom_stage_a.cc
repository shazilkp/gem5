#include "mem/cache/dom_stage_a.hh"

#include <cstdlib>
#include <cstring>

#include "base/logging.hh"
#include "mem/cache/cache_blk.hh"
#include "mem/cache/mshr.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

namespace
{

uint64_t
read_le(const uint8_t *data, unsigned size)
{
    uint64_t value = 0;
    const unsigned n = size < sizeof(value) ? size : sizeof(value);
    std::memcpy(&value, data, n);
    return value;
}

Addr
pc_or_zero(const Request *req)
{
    return req->hasPC() ? req->getPC() : 0;
}

} // anonymous namespace

DomStageAProbe &
DomStageAProbe::get()
{
    static DomStageAProbe probe;
    return probe;
}

DomStageAProbe::DomStageAProbe() : _enabled(false), _lines(0)
{
    const char *path = std::getenv("GEM5_DOM_STAGE_A_TRACE");
    if (path == nullptr || path[0] == '\0') {
        return;
    }

    _out.open(path, std::ios::out | std::ios::trunc);
    if (!_out.is_open()) {
        warn("dom_stage_a: cannot open %s; probe disabled\n", path);
        return;
    }
    _enabled = true;
}

DomStageAProbe::~DomStageAProbe()
{
    if (_enabled) {
        _out.flush();
        _out.close();
    }
}

void
DomStageAProbe::flush_if_needed()
{
    if ((++_lines & 0xFFF) == 0) {
        _out.flush();
    }
}

void
DomStageAProbe::log_load(uint64_t seq, uint64_t tid, Addr pc, Addr addr,
                         unsigned size, uint64_t value, Tick issue,
                         Tick latency, unsigned kind, uint64_t fill_seq,
                         uint64_t commit_seq, uint64_t fill_latency)
{
    _out << "L " << std::dec << seq << ' ' << tid
         << ' ' << std::hex << pc << ' ' << addr
         << ' ' << std::dec << size
         << ' ' << std::hex << value
         << ' ' << std::dec << issue << ' ' << latency << ' ' << kind
         << ' ' << std::hex << fill_seq << ' ' << commit_seq
         << ' ' << std::dec << fill_latency << '\n';
    flush_if_needed();
}

void
DomStageAProbe::log_store(PacketPtr pkt)
{
    const uint8_t *data = pkt->getPtr<uint8_t>();
    const uint64_t value = read_le(data, pkt->getSize());

    _out << "S " << curTick()
         << ' ' << std::hex << pc_or_zero(pkt->req.get())
         << ' ' << pkt->req->getVaddr()
         << ' ' << std::dec << pkt->getSize()
         << ' ' << std::hex << value << std::dec << '\n';
    flush_if_needed();
}

void
DomStageAProbe::request(PacketPtr pkt, bool hit, CacheBlk *blk,
                        unsigned blk_size)
{
    if (!_enabled || pkt == nullptr || pkt->req == nullptr) {
        return;
    }

    if (pkt->req->isInstFetch() || pkt->req->isPrefetch() ||
        pkt->req->isUncacheable()) {
        return;
    }

    const uint64_t seq = pkt->req->domSeq();
    const uint64_t commit_seq = pkt->req->domCommitSeq();

    if (pkt->isRead()) {
        if (hit) {
            if (blk != nullptr && seq != 0 &&
                blk->fillSeq() > commit_seq) {
                const unsigned off = pkt->getAddr() & (blk_size - 1);
                const uint64_t value =
                    read_le(blk->data + off, pkt->getSize());
                const Tick now = curTick();
                const Tick latency = blk->fillLatency();

                log_load(seq, pkt->req->contextId(),
                         pc_or_zero(pkt->req.get()), pkt->req->getVaddr(),
                         pkt->getSize(), value, now, latency, 1,
                         blk->fillSeq(), commit_seq, latency);
                _delayed.insert(seq);
            }
            return;
        }

        Pending p;
        p.tick = curTick();
        p.pc = pc_or_zero(pkt->req.get());
        p.addr = pkt->req->getVaddr();
        p.size = pkt->getSize();
        p.seq = seq;
        p.commit_seq = commit_seq;
        p.tid = pkt->req->contextId();
        _pending[pkt->req.get()] = p;
        if (seq != 0) {
            _pending_seqs.insert(seq);
        }
    } else if (pkt->isWrite() && !pkt->isEviction()) {
        log_store(pkt);
    }
}

void
DomStageAProbe::fill(PacketPtr pkt, MSHR *mshr, unsigned blk_size)
{
    if (!_enabled || pkt == nullptr || mshr == nullptr || !pkt->hasData()) {
        return;
    }

    const uint8_t *line = pkt->getPtr<uint8_t>();

    for (const auto &target : mshr->getTargets()) {
        if (target.pkt == nullptr || target.pkt->req == nullptr) {
            continue;
        }

        auto it = _pending.find(target.pkt->req.get());
        if (it == _pending.end()) {
            continue;
        }

        const Pending &p = it->second;
        const unsigned off = target.pkt->getAddr() & (blk_size - 1);
        const uint64_t value = read_le(line + off, p.size);
        const Tick now = curTick();
        const Tick latency = now - p.tick;

        log_load(p.seq, p.tid, p.pc, p.addr, p.size, value, p.tick,
                 latency, 0, p.seq, p.commit_seq, latency);

        if (p.seq != 0) {
            _pending_seqs.erase(p.seq);
            _delayed.insert(p.seq);
        }
        _pending.erase(it);
    }
}

void
DomStageAProbe::head(uint64_t seq, Tick tick)
{
    if (!_enabled || seq == 0) {
        return;
    }
    if (_pending_seqs.count(seq) || _delayed.count(seq)) {
        _out << "V " << std::dec << seq << ' ' << tick << '\n';
        flush_if_needed();
    }
}

void
DomStageAProbe::commit(uint64_t seq, Tick tick)
{
    if (!_enabled || seq == 0) {
        return;
    }
    _pending_seqs.erase(seq);
    if (_delayed.erase(seq)) {
        _out << "C " << std::dec << seq << ' ' << tick << '\n';
        flush_if_needed();
    }
}

void
DomStageAProbe::squash(uint64_t seq, Tick tick)
{
    if (!_enabled || seq == 0) {
        return;
    }
    _pending_seqs.erase(seq);
    if (_delayed.erase(seq)) {
        _out << "X " << std::dec << seq << ' ' << tick << '\n';
        flush_if_needed();
    }
}

} // namespace gem5
