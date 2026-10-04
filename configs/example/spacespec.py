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

"""Run a SE-mode binary with the SpaceSpec shadow cache enabled.

SpaceSpec (SafeSpec, Khasawneh et al., DAC'19) keeps speculative cache fills
out of the L1 data cache.  A speculative miss fill is diverted into a small
per-thread shadow cache and becomes resident in L1-D only when the load
commits; if the load is squashed first, the line is discarded and never
existed anywhere an attacker can time.

Usage -- all se.py options are accepted and forwarded::

    build/RISCV/gem5.opt configs/example/spacespec.py \\
        --cpu-type=O3CPU --caches --cmd=<elf> [se.py options...]

Requires ``--caches``: without a private L1-D there is nothing to protect.

Why the wiring is here and not in a --param
---------------------------------------------
``--param`` cannot instantiate SimObjects.  ``SimObject.apply_config`` execs the
command-line string with globals bound only to the node's *existing* child
SimObjects, and ``shadow_cache`` defaults to NULL, so no ``ShadowCache`` name is
in scope.  Worse, the CPU and its L1-D must reference the *same* object -- the
cache fills it, the pipeline releases from it -- and that cannot be expressed as
two independent parameter assignments.

Threat model
------------
``se.py --caches`` builds a private L1I and L1D and *no* L2 (``CacheConfig``
only creates ``system.l2`` under ``--l2cache``), so the L1-D is the only cache
that can leak a speculative line.  A full-system run with a private L2 would
need that removed, or the claim does not hold: a diverted fill still populates
the level below.

Environment
-----------
SPACESPEC_ENTRIES   shadow entries (default 64, SafeSpec Fig. 3)
SPACESPEC_LVP       0 disables the Load Value Predictor (default 1)
"""

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_CONFIGS = os.path.normpath(os.path.join(_HERE, ".."))
# "common" lives in configs/, so that is what must go on the path. se.py
# resolves it relative to its own directory, which is why importing it after
# this setup works.
sys.path.insert(0, _CONFIGS)

from common import CacheConfig  # noqa: E402
from m5.objects import ShadowCache  # noqa: E402

NUM_ENTRIES = int(os.environ.get("SPACESPEC_ENTRIES", "64"))
LVP_ENABLED = os.environ.get("SPACESPEC_LVP", "1") not in ("0", "false", "no")

_real_config_cache = CacheConfig.config_cache


def _spacespec_config_cache(options, system):
    """Build the caches, then attach a shadow cache to each L1-D and CPU."""
    _real_config_cache(options, system)

    if not options.caches:
        sys.exit("spacespec.py requires --caches: SpaceSpec protects a private "
                 "L1-D, and without one there is nothing to protect.")

    for cpu in system.cpu:
        shadow = ShadowCache(
            num_entries=NUM_ENTRIES,
            # Must match the cache block size; a mismatch is fatal.
            line_size=options.cacheline_size,
        )

        # The CPU releases from the shadow at commit and on squash.
        cpu.shadow_cache = shadow
        # The L1-D captures fills into it and installs promoted lines back.
        cpu.dcache.shadow_cache = shadow

        if not LVP_ENABLED and cpu.loadValuePredictor is not None:
            cpu.loadValuePredictor.enabled = False

        print(
            "[spacespec] cpu %d: ShadowCache(num_entries=%d, line_size=%d) "
            "lvp=%s"
            % (
                cpu.cpu_id,
                NUM_ENTRIES,
                options.cacheline_size,
                "on" if LVP_ENABLED else "off",
            )
        )


CacheConfig.config_cache = _spacespec_config_cache

# Hand control to the standard SE script, which will import this file's
# patched CacheConfig.  Use the real path rather than a bare "se.py": gem5
# matches sys.argv[0] against its deprecation shims and aborts if it looks like
# a deprecated script.
_SE = os.path.join(_CONFIGS, "deprecated", "example", "se.py")
sys.argv = [_SE] + sys.argv[1:]
with open(_SE) as _f:
    exec(compile(_f.read(), _SE, "exec"), {"__name__": "__main__",
                                           "__file__": _SE})