/*
 * Copyright (c) 2011-2012, 2014, 2016, 2017, 2019-2020, 2024 Arm Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * Copyright (c) 2022-2023 The University of Edinburgh
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * Copyright (c) 2011 Regents of the University of California
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

#include "cpu/o3/cpu.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "base/logging.hh"
#include "cpu/activity.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/checker/thread_context.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/lsq_unit.hh"
#include "cpu/o3/thread_context.hh"
#include "cpu/simple_thread.hh"
#include "cpu/thread_context.hh"
#include "debug/Activity.hh"
#include "debug/Drain.hh"
#include "debug/O3CPU.hh"
#include "debug/Quiesce.hh"
#include "enums/MemoryMode.hh"
#include "mem/cache/base.hh"
#include "sim/cur_tick.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/stat_control.hh"
#include "sim/system.hh"

namespace gem5
{

struct BaseCPUParams;

namespace o3
{

namespace
{

/** Optional provider fill/invalidate trace, enabled by GEM5_PROV_TRACE. */
FILE *
domProviderTrace()
{
    static FILE *trace = nullptr;
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        if (const char *path = std::getenv("GEM5_PROV_TRACE")) {
            trace = std::fopen(path, "w");
        }
    }
    return trace;
}

} // anonymous namespace

CPU::CPU(const BaseO3CPUParams &params)
    : BaseCPU(params),
      mmu(params.mmu),
      tickEvent([this] { tick(); }, "O3CPU tick", false, Event::CPU_Tick_Pri),
      threadExitEvent([this] { exitThreads(); }, "O3CPU exit threads", false,
                      Event::CPU_Exit_Pri),
#ifndef NDEBUG
      instcount(0),
#endif
      removeInstsThisCycle(false),
      bac(this, params),
      ftq(this, params),
      fetch(this, params),
      decode(this, params),
      rename(this, params),
      iew(this, params),
      commit(this, params),

      regFile(params.numPhysIntRegs, params.numPhysFloatRegs,
              params.numPhysVecRegs, params.numPhysVecPredRegs,
              params.numPhysMatRegs, params.numPhysCCRegs,
              params.isa[0]->regClasses()),

      freeList(name() + ".freelist", &regFile),

      rob(this, params),

      scoreboard(name() + ".scoreboard", regFile.totalNumPhysRegs()),

      isa(numThreads, NULL),

      timeBuffer(params.backComSize, params.forwardComSize),
      fetchQueue(params.backComSize, params.forwardComSize),
      decodeQueue(params.backComSize, params.forwardComSize),
      renameQueue(params.backComSize, params.forwardComSize),
      iewQueue(params.backComSize, params.forwardComSize),
      activityRec(name(), NumStages,
                  params.backComSize + params.forwardComSize, params.activity),

      globalSeqNum(1),
      globalFTSeqNum(1),
      system(params.system),
      lastRunningCycle(curCycle()),
      cpuStats(this)
{
    fatal_if(FullSystem && params.numThreads > 1,
            "SMT is not supported in O3 in full system mode currently.");

    fatal_if(!FullSystem && params.numThreads < params.workload.size(),
            "More workload items (%d) than threads (%d) on CPU %s.",
            params.workload.size(), params.numThreads, name());

    if (!params.switched_out) {
        _status = Running;
    } else {
        _status = SwitchedOut;
    }

    if (params.checker) {
        BaseCPU *temp_checker = params.checker;
        checker = dynamic_cast<Checker<DynInstPtr> *>(temp_checker);
        checker->setIcachePort(&fetch.getInstPort());
        checker->setSystem(params.system);
    } else {
        checker = NULL;
    }

    if (!FullSystem) {
        thread.resize(numThreads);
        tids.resize(numThreads);
    }

    // The stages also need their CPU pointer setup.  However this
    // must be done at the upper level CPU because they have pointers
    // to the upper level CPU, and not this CPU.

    // Set up Pointers to the activeThreads list for each stage
    bac.setActiveThreads(&activeThreads);
    fetch.setActiveThreads(&activeThreads);
    decode.setActiveThreads(&activeThreads);
    rename.setActiveThreads(&activeThreads);
    iew.setActiveThreads(&activeThreads);
    commit.setActiveThreads(&activeThreads);

    // Give each of the stages the time buffer they will use.
    bac.setTimeBuffer(&timeBuffer);
    fetch.setTimeBuffer(&timeBuffer);
    decode.setTimeBuffer(&timeBuffer);
    rename.setTimeBuffer(&timeBuffer);
    iew.setTimeBuffer(&timeBuffer);
    commit.setTimeBuffer(&timeBuffer);

    // Also setup each of the stages' queues.
    bac.setFetchTargetQueue(&ftq);
    fetch.setBACandFTQPtr(&bac, &ftq);
    fetch.setFetchQueue(&fetchQueue);
    decode.setFetchQueue(&fetchQueue);
    commit.setFetchQueue(&fetchQueue);
    decode.setDecodeQueue(&decodeQueue);
    rename.setDecodeQueue(&decodeQueue);
    rename.setRenameQueue(&renameQueue);
    iew.setRenameQueue(&renameQueue);
    iew.setIEWQueue(&iewQueue);
    commit.setIEWQueue(&iewQueue);
    commit.setRenameQueue(&renameQueue);

    commit.setIEWStage(&iew);
    rename.setIEWStage(&iew);
    rename.setCommitStage(&commit);

    ThreadID active_threads;
    if (FullSystem) {
        active_threads = 1;
    } else {
        active_threads = params.workload.size();

        if (active_threads > MaxThreads) {
            panic("Workload Size too large. Increase the 'MaxThreads' "
                  "constant in cpu/o3/limits.hh or edit your workload size.");
        }
    }

    // Make Sure That this a Valid Architeture
    assert(numThreads);
    const auto &regClasses = params.isa[0]->regClasses();

    panic_if(params.numPhysIntRegs <=
            numThreads * regClasses.at(IntRegClass)->numRegs() &&
            regClasses.at(IntRegClass)->numRegs() != 0,
            "Not enough physical registers, consider increasing "
            "numPhysIntRegs\n");
    panic_if(params.numPhysFloatRegs <=
            numThreads * regClasses.at(FloatRegClass)->numRegs() &&
            regClasses.at(FloatRegClass)->numRegs() != 0,
            "Not enough physical registers, consider increasing "
            "numPhysFloatRegs\n");
    panic_if(params.numPhysVecRegs <=
            numThreads * regClasses.at(VecRegClass)->numRegs() &&
            regClasses.at(VecRegClass)->numRegs() != 0,
            "Not enough physical registers, consider increasing "
            "numPhysVecRegs\n");
    panic_if(params.numPhysVecPredRegs <=
            numThreads * regClasses.at(VecPredRegClass)->numRegs() &&
            regClasses.at(VecPredRegClass)->numRegs() != 0,
            "Not enough physical registers, consider increasing "
            "numPhysVecPredRegs\n");
    panic_if(params.numPhysMatRegs <=
            numThreads * regClasses.at(MatRegClass)->numRegs() &&
            regClasses.at(MatRegClass)->numRegs() != 0,
            "Not enough physical registers, consider increasing "
            "numPhysMatRegs\n");
    panic_if(params.numPhysCCRegs <=
            numThreads * regClasses.at(CCRegClass)->numRegs() &&
            regClasses.at(CCRegClass)->numRegs() != 0,
            "Not enough physical registers, consider increasing "
            "numPhysCCRegs\n");

    // Just make this a warning and go ahead anyway, to keep from having to
    // add checks everywhere.
    warn_if(regClasses.at(CCRegClass)->numRegs() == 0 &&
            params.numPhysCCRegs != 0,
            "Non-zero number of physical CC regs specified, even though\n"
            "    ISA does not use them.");

    rename.setScoreboard(&scoreboard);
    iew.setScoreboard(&scoreboard);

    // Setup the rename map for whichever stages need it.
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        isa[tid] = params.isa[tid];
        commitRenameMap[tid].init(regClasses, &regFile, &freeList);
        renameMap[tid].init(regClasses, &regFile, &freeList);
    }

    // Initialize rename map to assign physical registers to the
    // architectural registers for active threads only.
    for (ThreadID tid = 0; tid < active_threads; tid++) {
        for (auto type = (RegClassType)0; type <= CCRegClass;
                type = (RegClassType)(type + 1)) {
            for (auto &id: *regClasses.at(type)) {
                // Note that we can't use the rename() method because we don't
                // want special treatment for the zero register at this point
                PhysRegIdPtr phys_reg = freeList.getReg(type);
                renameMap[tid].setEntry(id, phys_reg);
                commitRenameMap[tid].setEntry(id, phys_reg);
            }
        }
    }

    rename.setRenameMap(renameMap);
    commit.setRenameMap(commitRenameMap);
    rename.setFreeList(&freeList);

    // Setup the ROB for whichever stages need it.
    commit.setROB(&rob);

    lastActivatedCycle = 0;

    DPRINTF(O3CPU, "Creating O3CPU object.\n");

    // Setup any thread state.
    thread.resize(numThreads);

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (FullSystem) {
            // SMT is not supported in FS mode yet.
            assert(numThreads == 1);
            thread[tid] = new ThreadState(this, 0, NULL);
        } else {
            if (tid < params.workload.size()) {
                DPRINTF(O3CPU, "Workload[%i] process is %#x", tid,
                        thread[tid]);
                thread[tid] = new ThreadState(this, tid, params.workload[tid]);
            } else {
                //Allocate Empty thread so M5 can use later
                //when scheduling threads to CPU
                Process* dummy_proc = NULL;

                thread[tid] = new ThreadState(this, tid, dummy_proc);
            }
        }

        gem5::ThreadContext *tc;

        // Setup the TC that will serve as the interface to the threads/CPU.
        auto *o3_tc = new ThreadContext;

        tc = o3_tc;

        // If we're using a checker, then the TC should be the
        // CheckerThreadContext.
        if (params.checker) {
            tc = new CheckerThreadContext<ThreadContext>(o3_tc, checker);
        }

        o3_tc->cpu = this;
        o3_tc->thread = thread[tid];

        // Give the thread the TC.
        thread[tid]->tc = tc;

        // Add the TC to the CPU's list of TC's.
        threadContexts.push_back(tc);
    }

    // O3CPU always requires an interrupt controller.
    if (!params.switched_out && interrupts.empty()) {
        fatal("O3CPU %s has no interrupt controller.\n"
              "Ensure createInterruptController() is called.\n", name());
    }
}

void
CPU::regProbePoints()
{
    BaseCPU::regProbePoints();

    ppInstAccessComplete = new ProbePointArg<PacketPtr>(
            getProbeManager(), "InstAccessComplete");
    ppDataAccessComplete = new ProbePointArg<
        std::pair<DynInstPtr, PacketPtr>>(
                getProbeManager(), "DataAccessComplete");

    ftq.regProbePoints();
    bac.regProbePoints();
    fetch.regProbePoints();
    rename.regProbePoints();
    iew.regProbePoints();
    commit.regProbePoints();
}

CPU::CPUStats::CPUStats(CPU *cpu)
    : statistics::Group(cpu),
      ADD_STAT(timesIdled, statistics::units::Count::get(),
               "Number of times that the entire CPU went into an idle state "
               "and unscheduled itself"),
      ADD_STAT(idleCycles, statistics::units::Cycle::get(),
               "Total number of cycles that the CPU has spent unscheduled due "
               "to idling"),
      ADD_STAT(quiesceCycles, statistics::units::Cycle::get(),
               "Total number of cycles that CPU has spent quiesced or waiting "
               "for an interrupt"),
      ADD_STAT(domCastersC, statistics::units::Count::get(),
               "DoM control-flow (C) casters inserted"),
      ADD_STAT(domCastersE, statistics::units::Count::get(),
               "DoM memory/exception (E) casters inserted"),
      ADD_STAT(domProvHits, statistics::units::Count::get(),
               "DoM provider lookups that supplied a committed value"),
      ADD_STAT(domProvMisses, statistics::units::Count::get(),
               "DoM provider lookups that could not supply the value"),
      ADD_STAT(domProvFills, statistics::units::Count::get(),
               "DoM provider committed fills/updates"),
      ADD_STAT(domProvFlushes, statistics::units::Count::get(),
               "DoM provider flushes after syscall/DMA writes")
{
    // Register any of the O3CPU's stats here.
    timesIdled
        .prereq(timesIdled);

    idleCycles
        .prereq(idleCycles);

    quiesceCycles
        .prereq(quiesceCycles);
}

void
CPU::tick()
{
    DPRINTF(O3CPU, "\n\nO3CPU: Ticking main, O3CPU.\n");
    assert(!switchedOut());
    assert(drainState() != DrainState::Drained);

    ++baseStats.numCycles;
    updateCycleCounters(BaseCPU::CPU_STATE_ON);

//    activity = false;

    //Tick each of the stages
    bac.tick();

    fetch.tick();

    decode.tick();

    rename.tick();

    iew.tick();

    commit.tick();

    // Now advance the time buffers
    timeBuffer.advance();

    fetchQueue.advance();
    decodeQueue.advance();
    renameQueue.advance();
    iewQueue.advance();

    activityRec.advance();

    if (removeInstsThisCycle) {
        cleanUpRemovedInsts();
    }

    if (!tickEvent.scheduled()) {
        if (_status == SwitchedOut) {
            DPRINTF(O3CPU, "Switched out!\n");
            // increment stat
            lastRunningCycle = curCycle();
        } else if (!activityRec.active() || _status == Idle) {
            DPRINTF(O3CPU, "Idle!\n");
            lastRunningCycle = curCycle();
            cpuStats.timesIdled++;
        } else {
            schedule(tickEvent, clockEdge(Cycles(1)));
            DPRINTF(O3CPU, "Scheduling next tick!\n");
        }
    }

    if (!FullSystem)
        updateThreadPriority();

    tryDrain();
}

void
CPU::init()
{
    BaseCPU::init();

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        // Set noSquashFromTC so that the CPU doesn't squash when initially
        // setting up registers.
        thread[tid]->noSquashFromTC = true;
    }

    // Clear noSquashFromTC.
    for (int tid = 0; tid < numThreads; ++tid)
        thread[tid]->noSquashFromTC = false;

    commit.setThreads(thread);
}

void
CPU::startup()
{
    BaseCPU::startup();

    // Delay-on-Miss is a run-time gate so the same binary can produce both
    // the baseline and the DoM arm.
    domActive = (std::getenv("GEM5_DOM") != nullptr);
    if (domActive) {
        domCache = domResolveCache();
        if (domCache == nullptr) {
            warn("GEM5_DOM is set but the data port is not bound to a "
                 "BaseCache; disabling Delay-on-Miss");
            domActive = false;
        }
    }

    domProvActive = domActive && (std::getenv("GEM5_PROV") != nullptr);
    if (domProvActive) {
        std::size_t entries = 65536;
        if (const char *env = std::getenv("GEM5_PROV_ENTRIES")) {
            entries = std::strtoull(env, nullptr, 10);
        }

        const std::size_t ways = DomValueProvider::Ways;
        entries = std::max<std::size_t>(ways, entries);
        entries = (entries / ways) * ways;

        domProv = std::make_unique<DomValueProvider>(entries);

        if (const char *env = std::getenv("GEM5_PROV_LATENCY")) {
            domProvLatency = std::strtoul(env, nullptr, 10);
        }

        inform("DoM provider: %llu entries (%llu sets x %u ways), "
               "fixed latency %u cycles",
               (unsigned long long)entries,
               (unsigned long long)domProv->setCount(),
               DomValueProvider::Ways, domProvLatency);
    }

    bac.startupStage();
    fetch.startupStage();
    decode.startupStage();
    iew.startupStage();
    rename.startupStage();
    commit.startupStage();
}

BaseCache *
CPU::domResolveCache()
{
    if (!domCacheResolved) {
        domCacheResolved = true;
        RequestPort *port = dynamic_cast<RequestPort *>(&getDataPort());
        if (port != nullptr) {
            auto *cache_port = dynamic_cast<BaseCache::CpuSidePort *>(
                    port->getResponsePort());
            if (cache_port != nullptr) {
                domCache = &cache_port->getCache();
            }
        }
    }
    return domCache;
}

void
CPU::domInsert(const DynInstPtr &inst)
{
    DomShadowBuffer &sb = domSb[inst->threadNumber];

    // E-shadow: a memory access whose translation/fault status is not yet
    // final.  The cast is cleared when the access finishes translation, and
    // the entry leaves the SB at retirement.
    const bool mem_caster = inst->isMemRef() &&
        !(inst->isInstPrefetch() || inst->isDataPrefetch());

    // A load's shadow is the youngest caster that existed before it was
    // dispatched.  Compute it before adding the load's own E entry, otherwise
    // the load would shadow itself and never be released.
    if (inst->isLoad()) {
        inst->domCaster = sb.youngestCaster();
        panic_if(inst->domCaster >= inst->seqNum,
                 "DoM: load [sn:%llu] recorded a self/younger caster "
                 "[sn:%llu]", inst->seqNum, inst->domCaster);
    }

    if (mem_caster) {
        sb.add(inst->seqNum);
        ++cpuStats.domCastersE;
    } else if (inst->isControl() &&
               !(inst->isDirectCtrl() && inst->isUncondCtrl())) {
        // C-shadow: control flow whose direction or target is not known at
        // decode.  Direct unconditional jumps are already resolved.
        sb.add(inst->seqNum);
        ++cpuStats.domCastersC;
    }
}

bool
CPU::domShadowed(const DynInstPtr &inst) const
{
    panic_if(inst->domCaster >= inst->seqNum,
             "DoM: load [sn:%llu] is shadowed by self/younger [sn:%llu]",
             inst->seqNum, inst->domCaster);

    // A load stays shadowed until its caster retires and leaves the SB, not
    // merely until the caster's casts bit is cleared at verification.
    return inst->domCaster != 0 &&
           domSb[inst->threadNumber].contains(inst->domCaster);
}

bool
CPU::domCacheHit(Addr paddr, bool is_secure)
{
    // Without a cache every access is a miss; treat it as a hit so that a
    // misconfigured run cannot stall forever.
    return domCache == nullptr || domCache->inCache(paddr, is_secure);
}

void
CPU::domResolve(InstSeqNum seq, ThreadID tid)
{
    // Verification stops the instruction from casting a shadow on newly
    // dispatched loads.  Loads that already recorded it wait for retirement:
    // only then is it certain that every older caster has also retired.
    domSb[tid].resolve(seq);
}

void
CPU::domRetire(InstSeqNum seq, ThreadID tid)
{
    const auto retired = domSb[tid].retire(seq);
    for (InstSeqNum caster : retired) {
        iew.ldstQueue.releaseShadowed(caster);
        if (domCache != nullptr) {
            domCache->domRelease(caster);
        }
    }
}

void
CPU::domSquash(InstSeqNum seq, ThreadID tid)
{
    domSb[tid].squash(seq);

    if (domCache != nullptr) {
        domCache->domSquash(seq);
    }
}

bool
CPU::domProviderLookup(const RequestPtr &req, uint8_t *dst)
{
    if (!domProvActive || domProv == nullptr) {
        return false;
    }

    if (FILE *trace = domProviderTrace()) {
        std::fprintf(trace, "Q %llu %#llx %u\n",
                     (unsigned long long)req->domSeq(),
                     (unsigned long long)req->getPaddr(), req->getSize());
    }

    // Uncacheable, local (MMIO/IPR), and cache-maintenance requests must
    // always reach the device/cache and must never be served from the table.
    if (req->isUncacheable() || req->isLocalAccess() ||
        req->isCacheMaintenance()) {
        ++cpuStats.domProvMisses;
        return false;
    }

    if (domProv->lookup(req->getPaddr(), req->getSize(), dst)) {
        ++cpuStats.domProvHits;
        if (FILE *trace = domProviderTrace()) {
            std::fprintf(trace, "H %llu %#llx %u\n",
                         (unsigned long long)req->domSeq(),
                         (unsigned long long)req->getPaddr(), req->getSize());
        }
        return true;
    }

    ++cpuStats.domProvMisses;
    return false;
}

void
CPU::domProviderCommit(const DynInstPtr &inst)
{
    if (!domProvActive || domProv == nullptr) {
        return;
    }
    if (inst->getFault() != NoFault || !inst->effAddrValid()) {
        return;
    }

    RequestPtr req;
    if (inst->savedRequest != nullptr) {
        req = inst->savedRequest->mainReq();
    }

    const bool unsupported = req != nullptr &&
        (req->isUncacheable() || req->isLocalAccess() ||
         req->isCacheMaintenance());
    const bool no_data = req != nullptr &&
        (req->getFlags() & Request::STORE_NO_DATA);

    if (inst->isLoad()) {
        // Prefetches do not produce a committed architectural load, and
        // uncacheable/local/maintenance requests are not cacheable values.
        if (inst->isDataPrefetch() || inst->isInstPrefetch() || unsupported) {
            return;
        }
        if (inst->memData != nullptr) {
            domProv->fill(inst->physEffAddr, inst->memData, inst->effSize);
            ++cpuStats.domProvFills;
            if (FILE *trace = domProviderTrace()) {
                uint64_t value = 0;
                std::memcpy(&value, inst->memData,
                            std::min<unsigned>(inst->effSize, 8));
                std::fprintf(trace, "L %llu %#llx %u %016llx\n",
                             (unsigned long long)inst->seqNum,
                             (unsigned long long)inst->physEffAddr,
                             inst->effSize, (unsigned long long)value);
            }
        }
        return;
    }

    if (!inst->isStore()) {
        return;
    }

    // The architectural store data is the copy in the store queue;
    // DynInst::memData is not populated for stores.
    LSQUnit &store_unit = iew.ldstQueue.getLSQUnit(inst->threadNumber);
    const auto &store_entry = store_unit.storeQueue[inst->sqIdx];

    const bool masked = req != nullptr && req->isMasked();
    const unsigned store_size = store_entry.valid() ? store_entry.size() : 0;

    const bool unrepresentable =
        store_size > sizeof(uint64_t) ||
        ((inst->physEffAddr & (sizeof(uint64_t) - 1)) + store_size >
         sizeof(uint64_t));

    if (unsupported || no_data || inst->isAtomic() ||
        inst->isStoreConditional() || masked || !store_entry.valid() ||
        store_size == 0 || store_size != inst->effSize ||
        store_entry.isAllZeros() || unrepresentable) {
        // No single exact entry can describe this write.  Drop every word it
        // touches rather than leaving a stale committed value behind.
        domProv->invalidateRange(inst->physEffAddr, inst->effSize);
        if (FILE *trace = domProviderTrace()) {
            std::fprintf(trace, "I %llu %#llx %u\n",
                         (unsigned long long)inst->seqNum,
                         (unsigned long long)inst->physEffAddr,
                         inst->effSize);
        }
        return;
    }

    domProv->fill(inst->physEffAddr,
                  reinterpret_cast<const uint8_t *>(store_entry.data()),
                  store_size);
    ++cpuStats.domProvFills;
    if (FILE *trace = domProviderTrace()) {
        uint64_t value = 0;
        std::memcpy(&value, store_entry.data(),
                    std::min<unsigned>(store_size, 8));
        std::fprintf(trace, "S %llu %#llx %u %016llx\n",
                     (unsigned long long)inst->seqNum,
                     (unsigned long long)inst->physEffAddr,
                     inst->effSize, (unsigned long long)value);
    }
}

void
CPU::domProviderFlush()
{
    if (domProvActive && domProv != nullptr) {
        domProv->flush();
        ++cpuStats.domProvFlushes;
        if (FILE *trace = domProviderTrace()) {
            std::fprintf(trace, "X\n");
        }
    }
}

void
CPU::activateThread(ThreadID tid)
{
    auto active_it = std::find(
            activeThreads.begin(), activeThreads.end(), tid);

    DPRINTF(O3CPU, "[tid:%i] Calling activate thread.\n", tid);
    assert(!switchedOut());

    if (active_it == activeThreads.end()) {
        DPRINTF(O3CPU, "[tid:%i] Adding to active threads list\n", tid);

        activeThreads.push_back(tid);
    }
}

void
CPU::deactivateThread(ThreadID tid)
{
    // hardware transactional memory
    // shouldn't deactivate thread in the middle of a transaction
    assert(!commit.executingHtmTransaction(tid));

    //Remove From Active List, if Active
    auto active_it = std::find(
            activeThreads.begin(), activeThreads.end(), tid);

    DPRINTF(O3CPU, "[tid:%i] Calling deactivate thread.\n", tid);
    assert(!switchedOut());

    if (active_it != activeThreads.end()) {
        DPRINTF(O3CPU,"[tid:%i] Removing from active threads list\n",
                tid);
        activeThreads.erase(active_it);
    }

    bac.deactivateThread(tid);
    fetch.deactivateThread(tid);
    commit.deactivateThread(tid);
}

Counter
CPU::totalInsts() const
{
    Counter total(0);

    ThreadID size = thread.size();
    for (ThreadID i = 0; i < size; i++)
        total += thread[i]->numInst;

    return total;
}

Counter
CPU::totalOps() const
{
    Counter total(0);

    ThreadID size = thread.size();
    for (ThreadID i = 0; i < size; i++)
        total += thread[i]->numOp;

    return total;
}

void
CPU::activateContext(ThreadID tid)
{
    assert(!switchedOut());

    // Needs to set each stage to running as well.
    activateThread(tid);

    // We don't want to wake the CPU if it is drained. In that case,
    // we just want to flag the thread as active and schedule the tick
    // event from drainResume() instead.
    if (drainState() == DrainState::Drained)
        return;

    // If we are time 0 or if the last activation time is in the past,
    // schedule the next tick and wake up the fetch unit
    if (lastActivatedCycle == 0 || lastActivatedCycle < curTick()) {
        scheduleTickEvent(Cycles(0));

        // Be sure to signal that there's some activity so the CPU doesn't
        // deschedule itself.
        activityRec.activity();
        fetch.wakeFromQuiesce();

        Cycles cycles(curCycle() - lastRunningCycle);
        // @todo: This is an oddity that is only here to match the stats
        if (cycles != 0)
            --cycles;
        cpuStats.quiesceCycles += cycles;

        lastActivatedCycle = curTick();

        _status = Running;

        BaseCPU::activateContext(tid);
    }
}

void
CPU::suspendContext(ThreadID tid)
{
    DPRINTF(O3CPU,"[tid:%i] Suspending Thread Context.\n", tid);
    assert(!switchedOut());

    deactivateThread(tid);

    // If this was the last thread then unschedule the tick event.
    if (activeThreads.size() == 0) {
        unscheduleTickEvent();
        lastRunningCycle = curCycle();
        _status = Idle;
    }

    DPRINTF(Quiesce, "Suspending Context\n");

    BaseCPU::suspendContext(tid);
}

void
CPU::haltContext(ThreadID tid)
{
    //For now, this is the same as deallocate
    DPRINTF(O3CPU,"[tid:%i] Halt Context called. Deallocating\n", tid);
    assert(!switchedOut());

    deactivateThread(tid);
    removeThread(tid);

    // If this was the last thread then unschedule the tick event.
    if (activeThreads.size() == 0) {
        if (tickEvent.scheduled())
        {
            unscheduleTickEvent();
        }
        lastRunningCycle = curCycle();
        _status = Idle;
    }
    updateCycleCounters(BaseCPU::CPU_STATE_SLEEP);
}

void
CPU::insertThread(ThreadID tid)
{
    DPRINTF(O3CPU,"[tid:%i] Initializing thread into CPU");
    // Will change now that the PC and thread state is internal to the CPU
    // and not in the ThreadContext.
    gem5::ThreadContext *src_tc;
    if (FullSystem)
        src_tc = system->threads[tid];
    else
        src_tc = tcBase(tid);

    //Bind Int Regs to Rename Map
    const auto &regClasses = isa[tid]->regClasses();

    for (auto type = (RegClassType)0; type <= CCRegClass;
            type = (RegClassType)(type + 1)) {
        for (auto &id: *regClasses.at(type)) {
            PhysRegIdPtr phys_reg = freeList.getReg(type);
            renameMap[tid].setEntry(id, phys_reg);
            scoreboard.setReg(phys_reg);
        }
    }

    //Copy Thread Data Into RegFile
    //copyFromTC(tid);

    //Set PC/NPC/NNPC
    pcState(src_tc->pcState(), tid);

    src_tc->setStatus(gem5::ThreadContext::Active);

    activateContext(tid);

    //Reset ROB/IQ/LSQ Entries
    commit.rob->resetEntries();
}

void
CPU::removeThread(ThreadID tid)
{
    DPRINTF(O3CPU,"[tid:%i] Removing thread context from CPU.\n", tid);

    // Copy Thread Data From RegFile
    // If thread is suspended, it might be re-allocated
    // copyToTC(tid);


    // @todo: 2-27-2008: Fix how we free up rename mappings
    // here to alleviate the case for double-freeing registers
    // in SMT workloads.

    // clear all thread-specific states in each stage of the pipeline
    // since this thread is going to be completely removed from the CPU
    commit.clearStates(tid);
    bac.clearStates(tid);
    fetch.clearStates(tid);
    decode.clearStates(tid);
    rename.clearStates(tid);
    iew.clearStates(tid);

    // at this step, all instructions in the pipeline should be already
    // either committed successfully or squashed. All thread-specific
    // queues in the pipeline must be empty.
    assert(iew.instQueue.getCount(tid) == 0);
    assert(iew.ldstQueue.getCount(tid) == 0);
    assert(commit.rob->isEmpty(tid));
    assert(ftq.isEmpty(tid));

    // Reset ROB/IQ/LSQ Entries

    // Commented out for now.  This should be possible to do by
    // telling all the pipeline stages to drain first, and then
    // checking until the drain completes.  Once the pipeline is
    // drained, call resetEntries(). - 10-09-06 ktlim
/*
    if (activeThreads.size() >= 1) {
        commit.rob->resetEntries();
        iew.resetEntries();
    }
*/
}

Fault
CPU::getInterrupts()
{
    // Check if there are any outstanding interrupts
    return interrupts[0]->getInterrupt();
}

void
CPU::processInterrupts(const Fault &interrupt)
{
    // Check for interrupts here.  For now can copy the code that
    // exists within isa_fullsys_traits.hh.  Also assume that thread 0
    // is the one that handles the interrupts.
    // @todo: Possibly consolidate the interrupt checking code.
    // @todo: Allow other threads to handle interrupts.

    assert(interrupt != NoFault);
    interrupts[0]->updateIntrInfo();

    DPRINTF(O3CPU, "Interrupt %s being handled\n", interrupt->name());
    trap(interrupt, 0, nullptr);
}

void
CPU::trap(const Fault &fault, ThreadID tid, const StaticInstPtr &inst)
{
    // Pass the thread's TC into the invoke method.
    fault->invoke(threadContexts[tid], inst);
}

void
CPU::serializeThread(CheckpointOut &cp, ThreadID tid) const
{
    thread[tid]->serialize(cp);
}

void
CPU::unserializeThread(CheckpointIn &cp, ThreadID tid)
{
    thread[tid]->unserialize(cp);
}

DrainState
CPU::drain()
{
    // Deschedule any power gating event (if any)
    deschedulePowerGatingEvent();

    // If the CPU isn't doing anything, then return immediately.
    if (switchedOut())
        return DrainState::Drained;

    DPRINTF(Drain, "Draining...\n");

    // We only need to signal a drain to the commit stage as this
    // initiates squashing controls the draining. Once the commit
    // stage commits an instruction where it is safe to stop, it'll
    // squash the rest of the instructions in the pipeline and force
    // the fetch stage to stall. The pipeline will be drained once all
    // in-flight instructions have retired.
    commit.drain();

    // Wake the CPU and record activity so everything can drain out if
    // the CPU was not able to immediately drain.
    if (!isCpuDrained())  {
        // If a thread is suspended, wake it up so it can be drained
        for (auto t : threadContexts) {
            if (t->status() == gem5::ThreadContext::Suspended){
                DPRINTF(Drain, "Currently suspended so activate %i \n",
                        t->threadId());
                t->activate();
                // As the thread is now active, change the power state as well
                activateContext(t->threadId());
            }
        }

        wakeCPU();
        activityRec.activity();

        DPRINTF(Drain, "CPU not drained\n");

        return DrainState::Draining;
    } else {
        DPRINTF(Drain, "CPU is already drained\n");
        if (tickEvent.scheduled())
            deschedule(tickEvent);

        // Flush out any old data from the time buffers.  In
        // particular, there might be some data in flight from the
        // fetch stage that isn't visible in any of the CPU buffers we
        // test in isCpuDrained().
        for (int i = 0; i < timeBuffer.getSize(); ++i) {
            timeBuffer.advance();
            fetchQueue.advance();
            decodeQueue.advance();
            renameQueue.advance();
            iewQueue.advance();
        }

        drainSanityCheck();
        return DrainState::Drained;
    }
}

bool
CPU::tryDrain()
{
    if (drainState() != DrainState::Draining || !isCpuDrained())
        return false;

    if (tickEvent.scheduled())
        deschedule(tickEvent);

    DPRINTF(Drain, "CPU done draining, processing drain event\n");
    signalDrainDone();

    return true;
}

void
CPU::drainSanityCheck() const
{
    assert(isCpuDrained());
    bac.drainSanityCheck();
    fetch.drainSanityCheck();
    decode.drainSanityCheck();
    rename.drainSanityCheck();
    iew.drainSanityCheck();
    commit.drainSanityCheck();
}

bool
CPU::isCpuDrained() const
{
    bool drained(true);

    if (!instList.empty() || !removeList.empty()) {
        DPRINTF(Drain, "Main CPU structures not drained.\n");
        drained = false;
    }

    if (!bac.isDrained()) {
        DPRINTF(Drain, "BAC not drained.\n");
        drained = false;
    }

    if (!fetch.isDrained()) {
        DPRINTF(Drain, "Fetch not drained.\n");
        drained = false;
    }

    if (!decode.isDrained()) {
        DPRINTF(Drain, "Decode not drained.\n");
        drained = false;
    }

    if (!rename.isDrained()) {
        DPRINTF(Drain, "Rename not drained.\n");
        drained = false;
    }

    if (!iew.isDrained()) {
        DPRINTF(Drain, "IEW not drained.\n");
        drained = false;
    }

    if (!commit.isDrained()) {
        DPRINTF(Drain, "Commit not drained.\n");
        drained = false;
    }

    return drained;
}

void
CPU::commitDrained(ThreadID tid)
{
    bac.drainStall(tid);
    fetch.drainStall(tid);
}

void
CPU::drainResume()
{
    if (switchedOut())
        return;

    DPRINTF(Drain, "Resuming...\n");
    verifyMemoryMode();

    bac.drainResume();
    fetch.drainResume();
    commit.drainResume();

    _status = Idle;
    for (ThreadID i = 0; i < thread.size(); i++) {
        if (thread[i]->status() == gem5::ThreadContext::Active) {
            DPRINTF(Drain, "Activating thread: %i\n", i);
            activateThread(i);
            _status = Running;
        }
    }

    assert(!tickEvent.scheduled());
    if (_status == Running)
        schedule(tickEvent, nextCycle());

    // Reschedule any power gating event (if any)
    schedulePowerGatingEvent();
}

void
CPU::switchOut()
{
    DPRINTF(O3CPU, "Switching out\n");
    BaseCPU::switchOut();

    activityRec.reset();

    _status = SwitchedOut;

    if (checker)
        checker->switchOut();
}

void
CPU::takeOverFrom(BaseCPU *oldCPU)
{
    BaseCPU::takeOverFrom(oldCPU);

    bac.takeOverFrom();
    fetch.takeOverFrom();
    decode.takeOverFrom();
    rename.takeOverFrom();
    iew.takeOverFrom();
    commit.takeOverFrom();

    assert(!tickEvent.scheduled());

    auto *oldO3CPU = dynamic_cast<CPU *>(oldCPU);
    if (oldO3CPU)
        globalSeqNum = oldO3CPU->globalSeqNum;

    lastRunningCycle = curCycle();
    _status = Idle;
}

void
CPU::verifyMemoryMode() const
{
    if (!system->isTimingMode()) {
        fatal("The O3 CPU requires the memory system to be in "
              "'timing' mode.\n");
    }
}

RegVal
CPU::readMiscRegNoEffect(int misc_reg, ThreadID tid) const
{
    return isa[tid]->readMiscRegNoEffect(misc_reg);
}

RegVal
CPU::readMiscReg(int misc_reg, ThreadID tid)
{
    executeStats[tid]->numMiscRegReads++;
    return isa[tid]->readMiscReg(misc_reg);
}

void
CPU::setMiscRegNoEffect(int misc_reg, RegVal val, ThreadID tid)
{
    isa[tid]->setMiscRegNoEffect(misc_reg, val);
}

void
CPU::setMiscReg(int misc_reg, RegVal val, ThreadID tid)
{
    executeStats[tid]->numMiscRegWrites++;
    isa[tid]->setMiscReg(misc_reg, val);
}

RegVal
CPU::getReg(PhysRegIdPtr phys_reg, ThreadID tid)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        executeStats[tid]->numIntRegReads++;
        break;
      case FloatRegClass:
        executeStats[tid]->numFpRegReads++;
        break;
      case CCRegClass:
        executeStats[tid]->numCCRegReads++;
        break;
      case VecRegClass:
      case VecElemClass:
        executeStats[tid]->numVecRegReads++;
        break;
      case VecPredRegClass:
        executeStats[tid]->numVecPredRegReads++;
        break;
      default:
        break;
    }
    return regFile.getReg(phys_reg);
}

void
CPU::getReg(PhysRegIdPtr phys_reg, void *val, ThreadID tid)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        executeStats[tid]->numIntRegReads++;
        break;
      case FloatRegClass:
        executeStats[tid]->numFpRegReads++;
        break;
      case CCRegClass:
        executeStats[tid]->numCCRegReads++;
        break;
      case VecRegClass:
      case VecElemClass:
        executeStats[tid]->numVecRegReads++;
        break;
      case VecPredRegClass:
        executeStats[tid]->numVecPredRegReads++;
        break;
      default:
        break;
    }
    regFile.getReg(phys_reg, val);
}

void *
CPU::getWritableReg(PhysRegIdPtr phys_reg, ThreadID tid)
{
    switch (phys_reg->classValue()) {
      case VecRegClass:
        executeStats[tid]->numVecRegWrites++;
        break;
      case VecPredRegClass:
        executeStats[tid]->numVecPredRegWrites++;
        break;
      default:
        break;
    }
    return regFile.getWritableReg(phys_reg);
}

void
CPU::setReg(PhysRegIdPtr phys_reg, RegVal val, ThreadID tid)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        executeStats[tid]->numIntRegWrites++;
        break;
      case FloatRegClass:
        executeStats[tid]->numFpRegWrites++;
        break;
      case CCRegClass:
        executeStats[tid]->numCCRegWrites++;
        break;
      case VecRegClass:
      case VecElemClass:
        executeStats[tid]->numVecRegWrites++;
        break;
      case VecPredRegClass:
        executeStats[tid]->numVecPredRegWrites++;
        break;
      default:
        break;
    }
    regFile.setReg(phys_reg, val);
}

void
CPU::setReg(PhysRegIdPtr phys_reg, const void *val, ThreadID tid)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        executeStats[tid]->numIntRegWrites++;
        break;
      case FloatRegClass:
        executeStats[tid]->numFpRegWrites++;
        break;
      case CCRegClass:
        executeStats[tid]->numCCRegWrites++;
        break;
      case VecRegClass:
      case VecElemClass:
        executeStats[tid]->numVecRegWrites++;
        break;
      case VecPredRegClass:
        executeStats[tid]->numVecPredRegWrites++;
        break;
      default:
        break;
    }
    regFile.setReg(phys_reg, val);
}

RegVal
CPU::getArchReg(const RegId &reg, ThreadID tid)
{
    const RegId flat = reg.flatten(*isa[tid]);
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(flat);
    return regFile.getReg(phys_reg);
}

void
CPU::getArchReg(const RegId &reg, void *val, ThreadID tid)
{
    const RegId flat = reg.flatten(*isa[tid]);
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(flat);
    regFile.getReg(phys_reg, val);
}

void *
CPU::getWritableArchReg(const RegId &reg, ThreadID tid)
{
    const RegId flat = reg.flatten(*isa[tid]);
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(flat);
    return regFile.getWritableReg(phys_reg);
}

void
CPU::setArchReg(const RegId &reg, RegVal val, ThreadID tid)
{
    const RegId flat = reg.flatten(*isa[tid]);
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(flat);
    regFile.setReg(phys_reg, val);
}

void
CPU::setArchReg(const RegId &reg, const void *val, ThreadID tid)
{
    const RegId flat = reg.flatten(*isa[tid]);
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(flat);
    regFile.setReg(phys_reg, val);
}

const PCStateBase &
CPU::pcState(ThreadID tid)
{
    return commit.pcState(tid);
}

void
CPU::pcState(const PCStateBase &val, ThreadID tid)
{
    commit.pcState(val, tid);
}

void
CPU::squashFromTC(ThreadID tid)
{
    thread[tid]->noSquashFromTC = true;
    commit.generateTCEvent(tid);
}

CPU::ListIt
CPU::addInst(const DynInstPtr &inst)
{
    instList.push_back(inst);

    return --(instList.end());
}

void
CPU::instDone(ThreadID tid, const DynInstPtr &inst)
{
    // Keep an instruction count.
    if (!inst->isMicroop() || inst->isLastMicroop()) {
        thread[tid]->numInst++;
        thread[tid]->threadStats.numInsts++;
        commitStats[tid]->numInstsNotNOP++;

        // Check for instruction-count-based events.
        thread[tid]->comInstEventQueue.serviceEvents(thread[tid]->numInst);
    }

    if (inst->isMemRef()) {
        thread[tid]->threadStats.numMemRefs++;
    }

    thread[tid]->numOp++;
    thread[tid]->threadStats.numOps++;
    commitStats[tid]->numOpsNotNOP++;

    probeInstCommit(inst->staticInst, inst->pcState().instAddr());
}

void
CPU::removeFrontInst(const DynInstPtr &inst)
{
    DPRINTF(O3CPU, "Removing committed instruction [tid:%i] PC %s "
            "[sn:%lli]\n",
            inst->threadNumber, inst->pcState(), inst->seqNum);

    removeInstsThisCycle = true;

    // Remove the front instruction.
    removeList.push(inst->getInstListIt());
}

void
CPU::removeInstsNotInROB(ThreadID tid)
{
    DPRINTF(O3CPU, "Thread %i: Deleting instructions from instruction"
            " list.\n", tid);

    ListIt end_it;

    bool rob_empty = false;

    if (instList.empty()) {
        return;
    } else if (rob.isEmpty(tid)) {
        DPRINTF(O3CPU, "ROB is empty, squashing all insts.\n");
        end_it = instList.begin();
        rob_empty = true;
    } else {
        end_it = (rob.readTailInst(tid))->getInstListIt();
        DPRINTF(O3CPU, "ROB is not empty, squashing insts not in ROB.\n");
    }

    removeInstsThisCycle = true;

    ListIt inst_it = instList.end();

    inst_it--;

    // Walk through the instruction list, removing any instructions
    // that were inserted after the given instruction iterator, end_it.
    while (inst_it != end_it) {
        assert(!instList.empty());

        squashInstIt(inst_it, tid);

        inst_it--;
    }

    // If the ROB was empty, then we actually need to remove the first
    // instruction as well.
    if (rob_empty) {
        squashInstIt(inst_it, tid);
    }
}

void
CPU::removeInstsUntil(const InstSeqNum &seq_num, ThreadID tid)
{
    assert(!instList.empty());

    removeInstsThisCycle = true;

    ListIt inst_iter = instList.end();

    inst_iter--;

    DPRINTF(O3CPU, "Deleting instructions from instruction "
            "list that are from [tid:%i] and above [sn:%lli] (end=%lli).\n",
            tid, seq_num, (*inst_iter)->seqNum);

    while ((*inst_iter)->seqNum > seq_num) {

        bool break_loop = (inst_iter == instList.begin());

        squashInstIt(inst_iter, tid);

        inst_iter--;

        if (break_loop)
            break;
    }
}

void
CPU::squashInstIt(const ListIt &instIt, ThreadID tid)
{
    if ((*instIt)->threadNumber == tid) {
        DPRINTF(O3CPU, "Squashing instruction, "
                "[tid:%i] [sn:%lli] PC %s\n",
                (*instIt)->threadNumber,
                (*instIt)->seqNum,
                (*instIt)->pcState());

        // Mark it as squashed.
        (*instIt)->setSquashed();

        // @todo: Formulate a consistent method for deleting
        // instructions from the instruction list
        // Remove the instruction from the list.
        removeList.push(instIt);
    }
}

void
CPU::cleanUpRemovedInsts()
{
    while (!removeList.empty()) {
        DPRINTF(O3CPU, "Removing instruction, "
                "[tid:%i] [sn:%lli] PC %s\n",
                (*removeList.front())->threadNumber,
                (*removeList.front())->seqNum,
                (*removeList.front())->pcState());

        instList.erase(removeList.front());

        removeList.pop();
    }

    removeInstsThisCycle = false;
}
/*
void
CPU::removeAllInsts()
{
    instList.clear();
}
*/
void
CPU::dumpInsts()
{
    int num = 0;

    ListIt inst_list_it = instList.begin();

    cprintf("Dumping Instruction List\n");

    while (inst_list_it != instList.end()) {
        cprintf("Instruction:%i\nPC:%#x\n[tid:%i]\n[sn:%lli]\nIssued:%i\n"
                "Squashed:%i\n\n",
                num, (*inst_list_it)->pcState().instAddr(),
                (*inst_list_it)->threadNumber,
                (*inst_list_it)->seqNum, (*inst_list_it)->isIssued(),
                (*inst_list_it)->isSquashed());
        inst_list_it++;
        ++num;
    }
}
/*
void
CPU::wakeDependents(const DynInstPtr &inst)
{
    iew.wakeDependents(inst);
}
*/
void
CPU::wakeCPU()
{
    if (activityRec.active() || tickEvent.scheduled()) {
        DPRINTF(Activity, "CPU already running.\n");
        return;
    }

    DPRINTF(Activity, "Waking up CPU\n");

    Cycles cycles(curCycle() - lastRunningCycle);
    // @todo: This is an oddity that is only here to match the stats
    if (cycles > 1) {
        --cycles;
        cpuStats.idleCycles += cycles;
        baseStats.numCycles += cycles;
    }

    schedule(tickEvent, clockEdge());
}

void
CPU::wakeup(ThreadID tid)
{
    if (thread[tid]->status() != gem5::ThreadContext::Suspended)
        return;

    wakeCPU();

    DPRINTF(Quiesce, "Suspended Processor woken\n");
    threadContexts[tid]->activate();
}

ThreadID
CPU::getFreeTid()
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (!tids[tid]) {
            tids[tid] = true;
            return tid;
        }
    }

    return InvalidThreadID;
}

bool
CPU::inUserMode(ThreadID tid)
{
    return isa[tid]->inUserMode();
}

void
CPU::updateThreadPriority()
{
    if (activeThreads.size() > 1) {
        //DEFAULT TO ROUND ROBIN SCHEME
        //e.g. Move highest priority to end of thread list
        auto list_begin = activeThreads.begin();

        unsigned high_thread = *list_begin;

        activeThreads.erase(list_begin);

        activeThreads.push_back(high_thread);
    }
}

void
CPU::addThreadToExitingList(ThreadID tid)
{
    DPRINTF(O3CPU, "Thread %d is inserted to exitingThreads list\n", tid);

    // the thread trying to exit can't be already halted
    assert(tcBase(tid)->status() != gem5::ThreadContext::Halted);

    // make sure the thread has not been added to the list yet
    assert(exitingThreads.count(tid) == 0);

    // add the thread to exitingThreads list to mark that this thread is
    // trying to exit. The boolean value in the pair denotes if a thread is
    // ready to exit. The thread is not ready to exit until the corresponding
    // exit trap event is processed in the future. Until then, it'll be still
    // an active thread that is trying to exit.
    exitingThreads.emplace(std::make_pair(tid, false));
}

bool
CPU::isThreadExiting(ThreadID tid) const
{
    return exitingThreads.count(tid) == 1;
}

void
CPU::scheduleThreadExitEvent(ThreadID tid)
{
    assert(exitingThreads.count(tid) == 1);

    // exit trap event has been processed. Now, the thread is ready to exit
    // and be removed from the CPU.
    exitingThreads[tid] = true;

    // we schedule a threadExitEvent in the next cycle to properly clean
    // up the thread's states in the pipeline. threadExitEvent has lower
    // priority than tickEvent, so the cleanup will happen at the very end
    // of the next cycle after all pipeline stages complete their operations.
    // We want all stages to complete squashing instructions before doing
    // the cleanup.
    if (!threadExitEvent.scheduled()) {
        schedule(threadExitEvent, nextCycle());
    }
}

void
CPU::exitThreads()
{
    // there must be at least one thread trying to exit
    assert(exitingThreads.size() > 0);

    // terminate all threads that are ready to exit
    auto it = exitingThreads.begin();
    while (it != exitingThreads.end()) {
        ThreadID thread_id = it->first;
        bool readyToExit = it->second;

        if (readyToExit) {
            DPRINTF(O3CPU, "Exiting thread %d\n", thread_id);
            haltContext(thread_id);
            tcBase(thread_id)->setStatus(gem5::ThreadContext::Halted);
            it = exitingThreads.erase(it);
        } else {
            it++;
        }
    }
}

void
CPU::htmSendAbortSignal(ThreadID tid, uint64_t htm_uid,
        HtmFailureFaultCause cause)
{
    const Addr addr = 0x0ul;
    const int size = 8;
    const Request::Flags flags =
      Request::PHYSICAL|Request::STRICT_ORDER|Request::HTM_ABORT;

    // O3-specific actions
    iew.ldstQueue.resetHtmStartsStops(tid);
    commit.resetHtmStartsStops(tid);

    // notify l1 d-cache (ruby) that core has aborted transaction
    RequestPtr req =
        std::make_shared<Request>(addr, size, flags, _dataRequestorId);

    req->taskId(taskId());
    req->setContext(thread[tid]->contextId());
    req->setHtmAbortCause(cause);

    assert(req->isHTMAbort());

    PacketPtr abort_pkt = Packet::createRead(req);
    uint8_t *memData = new uint8_t[8];
    assert(memData);
    abort_pkt->dataStatic(memData);
    abort_pkt->setHtmTransactional(htm_uid);

    // TODO include correct error handling here
    if (!iew.ldstQueue.getDataPort().sendTimingReq(abort_pkt)) {
        panic("HTM abort signal was not sent to the memory subsystem.");
    }
}

} // namespace o3
} // namespace gem5
