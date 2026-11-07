#include "mem/cache/dom_stage_a.hh"

#include <cstdlib>
#include <cstring>

#include "base/logging.hh"
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
DomStageAProbe::request(PacketPtr pkt, bool hit)
{
    if (!_enabled || pkt == nullptr || pkt->req == nullptr) {
        return;
    }

    // The O3 LSQ does not populate Request::PC, so it is optional here.
    // Snoops are excluded by isInstFetch/eviction filters; Stage A is
    // single-core L1-only, so this is sufficient.
    if (pkt->req->isInstFetch() || pkt->req->isPrefetch() ||
        pkt->req->isUncacheable()) {
        return;
    }

    if (pkt->isRead()) {
        if (hit) {
            return; // v0 logs misses only
        }
        Pending p;
        p.tick = curTick();
        p.pc = pc_or_zero(pkt->req.get());
        p.addr = pkt->req->getVaddr();
        p.size = pkt->getSize();
        _pending[pkt->req.get()] = p;
    } else if (pkt->isWrite() && !pkt->isEviction()) {
        log_store(pkt);
    }
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
        // Page offset is identical in the virtual and physical address.
        const unsigned off = target.pkt->getAddr() & (blk_size - 1);
        const uint64_t value = read_le(line + off, p.size);
        const Tick latency = curTick() - p.tick;

        _out << "M " << curTick()
             << ' ' << std::hex << p.pc
             << ' ' << p.addr
             << ' ' << std::dec << p.size
             << ' ' << std::hex << value
             << ' ' << std::dec << latency << '\n';
        flush_if_needed();

        _pending.erase(it);
    }
}

} // namespace gem5
