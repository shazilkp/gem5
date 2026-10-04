# Copyright (c) 2026 SpaceSpec Authors
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.params import *
from m5.SimObject import SimObject


class ShadowCache(SimObject):
    """SpaceSpec shadow cache for one L1 data cache (SafeSpec, DAC'19).

    Holds speculatively fetched cache lines in isolation from the real L1-D.
    A line becomes resident only once the load that fetched it commits (the
    wait-for-commit policy); if the load is squashed first, the line is
    discarded and never existed anywhere an attacker can time.

    Attach the same instance to both the O3 CPU and its private L1-D.  The
    cache fills this and the CPU releases from it, so they must share one
    object.
    """

    type = "ShadowCache"
    cxx_header = "mem/cache/shadow_cache.hh"
    cxx_class = "gem5::ShadowCache"

    num_entries = Param.Unsigned(
        64,
        "Maximum shadow entries. SafeSpec Figure 3 suggests 64 covers the "
        "99.99th percentile of concurrent speculative d-cache accesses "
        "under the wait-for-commit policy.",
    )

    line_size = Param.Unsigned(
        64,
        "Cache line size in bytes. Must equal the guarded cache's block "
        "size; a mismatch is fatal.",
    )

    # The number of hardware threads is deliberately not a param.
    # CPU::numThreads is not visible to a SimObject default, so the CPU
    # reports it instead; a param here could silently disagree.