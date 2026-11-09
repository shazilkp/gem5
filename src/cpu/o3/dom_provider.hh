/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_DOM_PROVIDER_HH__
#define __CPU_O3_DOM_PROVIDER_HH__

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "base/types.hh"

namespace gem5
{

namespace o3
{

/**
 * Delay-on-Miss exact committed-value provider.
 *
 * A small, set-associative, byte-masked, per-word table of architecturally
 * committed values.  Populated from committed loads and stores and updated
 * by committed stores.  A shadowed L1 miss may be served from this table
 * instead of being dropped and re-issued.
 *
 * The table stores, for each word, the bytes whose committed value is known.
 * A lookup only succeeds when every requested byte is known.  Stores to a
 * word update exactly the bytes they wrote; bytes not written keep their
 * previously committed value.
 */
class DomValueProvider
{
  public:
    static constexpr unsigned Ways = 4;

    struct Entry
    {
        bool valid = false;
        uint64_t tag = 0;
        uint64_t value = 0;
        uint64_t mask = 0;
    };

    explicit DomValueProvider(std::size_t entries)
        : sets(std::max<std::size_t>(1, entries / Ways)),
          table(sets * Ways),
          nextWay(sets, 0)
    {
    }

    /**
     * Look up the committed bytes for [paddr, paddr+size).  On success,
     * copies them to dst and returns true.
     */
    bool
    lookup(Addr paddr, unsigned size, uint8_t *dst) const
    {
        if (size == 0 || size > sizeof(uint64_t)) {
            return false;
        }

        const unsigned offset = paddr & 7;
        if (offset + size > sizeof(uint64_t)) {
            return false;
        }

        const uint64_t mask = byteMask(offset, size);
        const uint64_t word = paddr >> 3;
        const std::size_t set = word % sets;

        for (unsigned way = 0; way < Ways; way++) {
            const Entry &entry = table[set * Ways + way];
            if (!entry.valid || entry.tag != word ||
                (entry.mask & mask) != mask) {
                continue;
            }

            const uint64_t shifted = entry.value >> (offset * 8);
            std::memcpy(dst, &shifted, size);
            return true;
        }
        return false;
    }

    /** Record committed bytes for [paddr, paddr+size). */
    void
    fill(Addr paddr, const uint8_t *src, unsigned size)
    {
        if (size == 0 || size > sizeof(uint64_t)) {
            return;
        }

        const unsigned offset = paddr & 7;
        if (offset + size > sizeof(uint64_t)) {
            return;
        }

        const uint64_t mask = byteMask(offset, size);
        const uint64_t word = paddr >> 3;
        const std::size_t set = word % sets;

        Entry *entry = nullptr;
        for (unsigned way = 0; way < Ways; way++) {
            Entry &candidate = table[set * Ways + way];
            if (candidate.valid && candidate.tag == word) {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr) {
            for (unsigned way = 0; way < Ways; way++) {
                Entry &candidate = table[set * Ways + way];
                if (!candidate.valid) {
                    entry = &candidate;
                    break;
                }
            }
        }
        if (entry == nullptr) {
            entry = &table[set * Ways + nextWay[set]];
            nextWay[set] = (nextWay[set] + 1) % Ways;
        }

        uint64_t value = 0;
        std::memcpy(&value, src, size);

        entry->valid = true;
        entry->tag = word;
        entry->value = (entry->value & ~mask) |
                       ((value << (offset * 8)) & mask);
        entry->mask |= mask;
    }

    std::size_t
    entryCount() const
    {
        return table.size();
    }

    std::size_t
    setCount() const
    {
        return sets;
    }

  private:
    static uint64_t
    byteMask(unsigned offset, unsigned size)
    {
        const uint64_t bytes = (size == sizeof(uint64_t))
            ? ~uint64_t(0)
            : ((uint64_t(1) << (size * 8)) - 1);
        return bytes << (offset * 8);
    }

    const std::size_t sets;
    std::vector<Entry> table;
    /** Round-robin replacement cursor per set. */
    std::vector<unsigned> nextWay;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_DOM_PROVIDER_HH__
