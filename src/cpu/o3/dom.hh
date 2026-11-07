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

#ifndef __CPU_O3_DOM_HH__
#define __CPU_O3_DOM_HH__

#include <deque>

#include "cpu/inst_seq.hh"

namespace gem5
{

namespace o3
{

/**
 * Delay-on-Miss shadow buffer (ISCA'19, Sakalis et al.).
 *
 * The buffer keeps the shadow-casting instructions in dispatch order.  In
 * this milestone only control-flow (C) shadows are implemented: control
 * instructions whose direction or target is not known at decode cast a
 * shadow until they are verified.  A load records the youngest caster present
 * when it is dispatched.  The load may proceed normally on an L1 hit; a
 * shadowed L1 miss is dropped and only re-issued once the caster is verified
 * (resolved).
 *
 * The buffer only tracks casters, not every instruction.  This is sufficient
 * for the C-shadow: a caster's retirement or squash is always ordered with
 * respect to the instructions it shadows, so removing it from the buffer is
 * enough to make younger loads non-speculative.
 */
class DomShadowBuffer
{
  public:
    struct Entry
    {
        InstSeqNum seq;
        bool casts;
    };

    /** Register a newly dispatched branch as a caster. */
    void
    add(InstSeqNum seq)
    {
        entries.push_back({seq, true});
    }

    /**
     * The youngest still-casting instruction at the current dispatch point,
     * or 0 if the load is not shadowed.
     */
    InstSeqNum
    youngestCaster() const
    {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->casts) {
                return it->seq;
            }
        }
        return 0;
    }

    /** Is the given sequence number still casting a shadow? */
    bool
    isCasting(InstSeqNum seq) const
    {
        for (const auto &entry : entries) {
            if (entry.seq == seq) {
                return entry.casts;
            }
        }
        return false;
    }

    /**
     * Verify a caster.  Returns true if it was still casting, i.e. if the
     * loads that recorded it as their caster can be released.
     */
    bool
    resolve(InstSeqNum seq)
    {
        for (auto &entry : entries) {
            if (entry.seq == seq) {
                if (!entry.casts) {
                    return false;
                }
                entry.casts = false;
                return true;
            }
        }
        return false;
    }

    /** Drop all casters at or below the retired instruction. */
    void
    retire(InstSeqNum seq)
    {
        while (!entries.empty() && entries.front().seq <= seq) {
            entries.pop_front();
        }
    }

    /** Drop every caster younger than the squash point. */
    void
    squash(InstSeqNum seq)
    {
        while (!entries.empty() && entries.back().seq > seq) {
            entries.pop_back();
        }
    }

    bool
    empty() const
    {
        return entries.empty();
    }

    std::size_t
    size() const
    {
        return entries.size();
    }

  private:
    std::deque<Entry> entries;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_DOM_HH__
