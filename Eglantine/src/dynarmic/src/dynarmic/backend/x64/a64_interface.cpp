#include <chrono>
#include <cstdio>
extern "C" void eden_jit_evacuation(unsigned core) __attribute__((weak));
extern "C" void eden_jit_compile(unsigned core, unsigned long long ns) __attribute__((weak));
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>
#include "dynarmic/backend/x64/exclusive_monitor_friend.h"
#include "dynarmic/backend/x64/jit_group.h"
extern "C" void eden_jit_phases(unsigned core, unsigned long long translate, unsigned long long optimize, unsigned long long emit, unsigned long long location) __attribute__((weak));
extern "C" unsigned long long eden_jit_clock_ns() __attribute__((weak));
#if defined(EDEN_DEV_PROFILE) // PS5 development build
extern "C" void eden_jit_phases(unsigned core, unsigned long long translate, unsigned long long optimize, unsigned long long emit, unsigned long long location) __attribute__((weak));
#else // PS5
#endif // PS5
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstring>
#include <memory>
#include <mutex>

#include <boost/icl/interval_set.hpp>
#include "common/assert.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include <bit>

#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opt_passes.h"

namespace Dynarmic::A64 {

using namespace Backend::X64;

static RunCodeCallbacks GenRunCodeCallbacks(A64::UserCallbacks* cb, CodePtr (*LookupBlock)(void* lookup_block_arg), void* arg, const A64::UserConfig& conf) {
    return RunCodeCallbacks{
        ArgCallback(LookupBlock, reinterpret_cast<u64>(arg)),
        ArgCallback(Devirtualize<&A64::UserCallbacks::AddTicks>(cb)),
        ArgCallback(Devirtualize<&A64::UserCallbacks::GetTicksRemaining>(cb)),
        conf.enable_cycle_counting,
        HostCallReturn(conf) ? std::optional<u32>{u32(offsetof(A64JitState, host_stack_base))} : std::nullopt,
    };
}

static std::function<void(BlockOfCode&)> GenRCP(const A64::UserConfig& conf) {
    return [conf](BlockOfCode& code) {
        if (conf.page_table) {
            code.mov(code.r14, std::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.mov(code.r13, *conf.fastmem_pointer);
        }
        if (HostCallReturn(conf)) {
            // A RET compares its target with the frame's [rsp + ABI_SHADOW_SPACE]; with no guest
            // frame that is the first StackLayout word (the spills live in the JIT state), which
            // holds a value no location descriptor has (bits 57-58 are never set in one).
            code.mov(code.qword[code.rsp + ABI_SHADOW_SPACE], -1);
            code.mov(code.qword[code.ABI_JIT_PTR + offsetof(A64JitState, host_stack_base)], code.rsp);
            code.lea(code.rax, code.ptr[code.rsp - HostCallFrameSize * HostCallMaxDepth]);
            code.mov(code.qword[code.ABI_JIT_PTR + offsetof(A64JitState, host_stack_limit)], code.rax);
        }
    };
}

// Largest power of two <= conf.rsb_size, between 8 and A64JitState::RSB_SIZE, minus one.
static size_t RsbPtrMask(const A64::UserConfig& conf) {
    size_t size = A64JitState::RSB_SIZE;
    while (size > 8 && size > conf.rsb_size) {
        size >>= 1;
    }
    return size - 1;
}

static Optimization::PolyfillOptions GenPolyfillOptions(const BlockOfCode& code) {
    return Optimization::PolyfillOptions{
        .sha256 = !code.HasHostFeature(HostFeature::SHA),
        .vector_multiply_widen = true,
    };
}

// EglantinePS5: JitGroup membership (jit_group.h) and the prelude callbacks of a JIT whose code
// every core of its process may run.
extern "C" void eden_state_arg_add(u64 value, u32 offset);  // callback.cpp
extern "C" void eden_state_arg_remove(u64 value);
extern "C" {
// Development switch (dev-settings jit_shared=off): every core keeps its own blocks.
bool eden_jit_shared = true;
}

namespace {
// Dispatch-path counters per guest core, read by the development performance report
// (eden_jit_path_counters): Run entries, dispatcher lookups from generated code, block lookups
// and hits, contended group locks, whole and targeted fast-dispatch flushes.
enum EdenPath : unsigned { EdenRuns, EdenThunks, EdenLookups, EdenHits, EdenContended, EdenFlushAll, EdenFlushLocations, EdenPathCount };
struct alignas(64) EdenPathCounters {
    std::array<std::atomic<unsigned long long>, EdenPathCount> values{};
};
std::array<EdenPathCounters, 4> eden_path{};
void EdenCount(u32 core, EdenPath what, unsigned long long amount = 1) {
    eden_path[core < eden_path.size() ? core : eden_path.size() - 1].values[what].fetch_add(amount, std::memory_order_relaxed);
}
std::mutex eden_groups_mutex;
std::vector<std::weak_ptr<JitGroup>> eden_groups;

std::shared_ptr<JitGroup> EdenJoinGroup(const void* key, void* member) {
    std::lock_guard lock{eden_groups_mutex};
    std::erase_if(eden_groups, [](const std::weak_ptr<JitGroup>& weak) { return weak.expired(); });
    std::shared_ptr<JitGroup> group;
    for (const auto& weak : eden_groups) {
        if (auto candidate = weak.lock(); candidate && candidate->key == key) {
            group = std::move(candidate);
            break;
        }
    }
    if (!group) {
        group = std::make_shared<JitGroup>();
        group->key = key;
        eden_groups.push_back(group);
    }
    std::lock_guard members{group->mutex};
    group->members.push_back(member);
    return group;
}

void EdenLeaveGroup(JitGroup& group, void* member) {
    std::lock_guard lock{group.mutex};
    std::erase(group.members, member);
}

}  // namespace

namespace {
// The application's group: the shared one with the most member cores (system processes and
// unshared JITs have groups of their own).
std::shared_ptr<JitGroup> EdenApplicationGroup() {
    std::lock_guard lock{eden_groups_mutex};
    std::shared_ptr<JitGroup> best;
    std::size_t most = 1;
    for (const auto& weak : eden_groups) {
        auto group = weak.lock();
        if (!group)
            continue;
        std::lock_guard members{group->mutex};
        if (group->members.size() > most) {
            most = group->members.size();
            best = std::move(group);
        }
    }
    return best;
}
}  // namespace

// Precompilation for a host thread that is no guest core (headless/jit_list.h).
//
// eden_jit_list_open: a handle on the application's compiled blocks, or null when there is no
// shared application group (yet). Close it with eden_jit_list_close.
extern "C" void* eden_jit_list_open() {
    std::shared_ptr<JitGroup> group = EdenApplicationGroup();
    return group ? new std::shared_ptr<JitGroup>(std::move(group)) : nullptr;
}

extern "C" void eden_jit_list_close(void* handle) {
    delete static_cast<std::shared_ptr<JitGroup>*>(handle);
}

// Compiles the block at `location` (a location descriptor's value) into the member region with
// the most room that its core is not compiling in, so that the cores keep equal headroom for
// their own compilations and never wait. 1: compiled; 0: nothing to do (already compiled, or a
// clear is under way); 2: every usable region is busy, try this location again; -1: stop (the
// game's JITs are gone, or every region is at its reserve).
extern "C" int eden_jit_precompile(void* handle, unsigned long long location) {
    JitGroup* const group = handle ? static_cast<std::shared_ptr<JitGroup>*>(handle)->get() : nullptr;
    if (!group)
        return -1;
    std::lock_guard precompiling{group->precompile_mutex};
    std::array<void*, 8> members{};
    std::size_t count = 0;
    int (*compile)(void*, u64) = nullptr;
    std::size_t (*space)(void*) = nullptr;
    {
        std::lock_guard lock{group->mutex};
        for (void* member : group->members)
            if (count < members.size())
                members[count++] = member;
        compile = group->precompile;
        space = group->space;
    }
    if (!compile || !space || count == 0)
        return -1;
    // Most room first. The room is read once: it changes while the cores compile.
    std::array<std::pair<std::size_t, void*>, 8> order{};
    for (std::size_t i = 0; i < count; ++i)
        order[i] = {space(members[i]), members[i]};
    std::sort(order.begin(), order.begin() + count,
              [](const auto& a, const auto& b) { return a.first > b.first; });
    bool busy = false;
    for (std::size_t i = 0; i < count; ++i) {
        const int result = compile(order[i].second, location);
        if (result >= 0)
            return result;
        busy |= result == -3;
    }
    return busy ? 2 : -1;
}

// The application's published locations so far, oldest first: copies up to `capacity` of them and
// returns how many there are.
extern "C" std::size_t eden_jit_history(void* handle, unsigned long long* out, std::size_t capacity) {
    JitGroup* const group = handle ? static_cast<std::shared_ptr<JitGroup>*>(handle)->get() : nullptr;
    if (!group)
        return 0;
    std::lock_guard lock{group->mutex};
    const std::size_t count = std::min(capacity, group->history.size());
    for (std::size_t i = 0; i < count; ++i)
        out[i] = group->history[i];
    return group->history.size();
}

extern "C" void eden_jit_path_counters(unsigned core, unsigned long long* out) {
    if (core >= eden_path.size()) return;
    for (unsigned i = 0; i < EdenPathCount; ++i)
        out[i] = eden_path[core].values[i].load(std::memory_order_relaxed);
    out[EdenPathCount] = JitGroup::far_links.load(std::memory_order_relaxed);
}

namespace {
// Registers the JIT object and callbacks before the prelude is generated, so that its dispatcher
// and callback calls load the running core's own from the JIT state (callback.cpp).
RunCodeCallbacks EdenRunCodeCallbacks(A64::UserCallbacks* cb, CodePtr (*lookup_block)(void*), void* arg,
                                      const A64::UserConfig& conf) {
    eden_state_arg_add(reinterpret_cast<u64>(cb), u32(offsetof(A64JitState, eden_callbacks)));
    eden_state_arg_add(reinterpret_cast<u64>(arg), u32(offsetof(A64JitState, eden_jit)));
    return GenRunCodeCallbacks(cb, lookup_block, arg, conf);
}
}  // namespace


// The PS5 app's native-sample hook (headless/mkw_native_sample.inc). The host test suites link the
// same JIT without the app: a weak no-op stands in, and the app's definition replaces it.
extern "C" __attribute__((weak)) void mkw_native_block(unsigned long long, const void*, unsigned long long) {}
// EglantinePS5: Jit::Impl with one compiled-code namespace per guest process (inserted into
// a64_interface.cpp in place of upstream's Impl; see headless/dynarmic/jit_group.h).
//
// Each core compiles into its own region, in parallel, and publishes the block in its process's
// JitGroup; every core then runs it. Measured in a large open-world game before this change: 992k
// compilations for 518k distinct blocks (48% compiled again by another core), 60-70% of the
// compilations in a gameplay stutter were such duplicates, and the per-core caches (256/192/192
// MiB) ran close to full while together holding each block up to three times.
//
// Protocol (all atomics sequentially consistent):
// - eden_state: Outside (not in Run/Step), Running (in generated code, including its callbacks),
//   Compiling (translating/emitting into this core's region), Lookup (in the dispatcher or at
//   Run entry, in C++, not touching the region).
// - A full clear (a region is full, or ClearCache) sets group->clearing, halts every member and
//   waits until none is Running or Compiling; only then are all regions reset, the group
//   forgotten and group->epoch advanced. Cores park in Lookup while clearing is set.
// - A code pointer obtained in Lookup is used only if, after switching to Running, no clear is in
//   progress and the epoch is unchanged; otherwise the lookup is repeated.
// - A range invalidation unlinks and forgets the affected blocks at once (their code stays until
//   the next full clear, so a core still inside one finishes it), then every member drops its
//   return stack buffer and their fast dispatch entries on its own thread before running again.
// - A host thread that is no guest core may compile into a core's region (EdenPrecompile, for a
//   saved block list). eden_compile_mutex serializes it with the core's own compilations, which
//   use the same translator, emitter and region; eden_pre_state is its Compiling mark, which a
//   clear waits for like a core's. It never clears: it stops while the region has a reserve left.
//   The core comes first: it announces itself in eden_compile_waiters before it takes the mutex,
//   and the precompiler only tries the mutex, and only while no core is announced, so a core
//   waits for at most the one block being emitted.
// The Impl running guest code on this thread, for the fastmem fault callback
// (headless/dynarmic/fastmem-callback.cmake). A function pointer, as the callback cannot name Impl.
static thread_local void* eden_running_impl = nullptr;
static bool (*eden_invalidate_running_hook)(u64, u64) = nullptr;
// Range invalidations that found no block (a demoted site whose block was not where its PC is).
static std::atomic<unsigned long long> eden_empty_invalidations{0};
struct Jit::Impl final {
public:
    // A fastmem site demoted on this thread: its block is invalidated like a code change, at this
    // core's next Run boundary.
    // The guest code did not change, so the other cores are not halted for it: they drop the block
    // at their next Run boundary, and one still inside it faults again and takes the fallback.
    // Halting every core for each of a first session's demotions made it slower than no window.
    static bool EdenInvalidateRunning(u64 pc, u64 location) {
        auto* const impl = static_cast<Impl*>(eden_running_impl);
        if (!impl) return false;
        {
            std::unique_lock lock{impl->invalidation_mutex};
            impl->invalid_cache_ranges.add(boost::icl::discrete_interval<u64>::closed(pc, pc + 3));
            impl->eden_demoted_locations.push_back(location);
            impl->eden_quiet_invalidation = true;
        }
        impl->HaltExecution(HaltReason::CacheInvalidation);
        return true;
    }
    Impl(Jit* jit, UserConfig conf)
        : conf(conf)
        , block_of_code(EdenRunCodeCallbacks(conf.callbacks, &GetCurrentBlockThunk, this, conf), JitStateInfo{jit_state, RsbPtrMask(conf)}, conf.code_cache_size, GenRCP(conf))
        , emitter(block_of_code, conf, jit)
        , polyfill_options(GenPolyfillOptions(block_of_code))
    {
        ASSERT(conf.page_table_address_space_bits >= 12 && conf.page_table_address_space_bits <= 64);
        // Cores share only when they share an address space and link blocks without cycle counting.
        const bool shared = eden_jit_shared && conf.page_table && !conf.enable_cycle_counting;
        group = EdenJoinGroup(shared ? static_cast<const void*>(conf.page_table) : this, this);
        emitter.eden_group = group.get();
        eden_invalidate_running_hook = &EdenInvalidateRunning;
        {
            std::lock_guard lock{group->mutex};
            group->precompile = &EdenPrecompileThunk;
            group->space = &EdenSpaceThunk;
        }
        eden_space.store(block_of_code.SpaceRemaining(), std::memory_order_relaxed);
        EdenInitState();
    }

    ~Impl() {
        {
            // A precompilation in this region finishes before the region goes.
            std::lock_guard precompiling{group->precompile_mutex};
            EdenLeaveGroup(*group, this);
        }
        eden_state_arg_remove(reinterpret_cast<u64>(conf.callbacks));
        eden_state_arg_remove(reinterpret_cast<u64>(this));
    }

    HaltReason Run() {
        MkwJitTimer mkw_run{18 + unsigned(conf.processor_id)};
        ASSERT(!is_executing);
        EdenCount(conf.processor_id, EdenRuns);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        is_executing = true;
        const CodePtr current_code_ptr = EdenEnter([this] {
            // RSB optimization
            const u32 new_rsb_ptr = (jit_state.rsb_ptr - 1) & u32(RsbPtrMask(conf));
            if (jit_state.GetUniqueHash() == jit_state.rsb_location_descriptors[new_rsb_ptr]) {
                jit_state.rsb_ptr = new_rsb_ptr;
                return CodePtr(jit_state.rsb_codeptrs[new_rsb_ptr]);
            }
            return CodePtr((uintptr_t(GetCurrentBlock()) + 15) & ~uintptr_t(15));
        });
        eden_running_impl = this;
        const HaltReason hr = block_of_code.RunCode(&jit_state, current_code_ptr);
        eden_running_impl = nullptr;
        eden_state.store(EdenOutside, std::memory_order_seq_cst);
        PerformRequestedCacheInvalidation(hr);
        is_executing = false;
        return hr;
    }

    HaltReason Step() {
        ASSERT(!is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        is_executing = true;
        const CodePtr code_ptr = EdenEnter([this] { return GetCurrentSingleStep(); });
        eden_running_impl = this;
        const HaltReason hr = block_of_code.StepCode(&jit_state, code_ptr);
        eden_running_impl = nullptr;
        eden_state.store(EdenOutside, std::memory_order_seq_cst);
        PerformRequestedCacheInvalidation(hr);
        is_executing = false;
        return hr;
    }

    void ClearCache() {
        std::unique_lock lock{invalidation_mutex};
        invalidate_entire_cache = true;
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void InvalidateCacheRange(u64 start_address, size_t length) {
        std::unique_lock lock{invalidation_mutex};
        const auto end_address = static_cast<u64>(start_address + length - 1);
        const auto range = boost::icl::discrete_interval<u64>::closed(start_address, end_address);
        invalid_cache_ranges.add(range);
        eden_quiet_invalidation = false;
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void Reset() {
        ASSERT(!is_executing);
        jit_state = {};
        EdenInitState();
    }

    void HaltExecution(HaltReason hr) {
        Atomic::Or(&jit_state.halt_reason, static_cast<u32>(hr));
    }

    void ClearHalt(HaltReason hr) {
        Atomic::And(&jit_state.halt_reason, ~static_cast<u32>(hr));
    }

    u64 GetSP() const {
        return jit_state.sp;
    }

    void SetSP(u64 value) {
        jit_state.sp = value;
    }

    u64 GetPC() const {
        return jit_state.pc;
    }

    void SetPC(u64 value) {
        jit_state.pc = value;
    }

    u64 GetRegister(size_t index) const {
        if (index == 31)
            return GetSP();
        return jit_state.reg.at(index);
    }

    void SetRegister(size_t index, u64 value) {
        if (index == 31)
            return SetSP(value);
        jit_state.reg.at(index) = value;
    }

    std::array<u64, 31> GetRegisters() const {
        return jit_state.reg;
    }

    void SetRegisters(const std::array<u64, 31>& value) {
        jit_state.reg = value;
    }

    Vector GetVector(size_t index) const {
        return {jit_state.vec.at(index * 2), jit_state.vec.at(index * 2 + 1)};
    }

    void SetVector(size_t index, Vector value) {
        jit_state.vec.at(index * 2) = value[0];
        jit_state.vec.at(index * 2 + 1) = value[1];
    }

    std::array<Vector, 32> GetVectors() const {
        std::array<Vector, 32> ret;
        static_assert(sizeof(ret) == sizeof(jit_state.vec));
        std::memcpy(ret.data(), jit_state.vec.data(), sizeof(jit_state.vec));
        return ret;
    }

    void SetVectors(const std::array<Vector, 32>& value) {
        static_assert(sizeof(value) == sizeof(jit_state.vec));
        std::memcpy(jit_state.vec.data(), value.data(), sizeof(jit_state.vec));
    }

    u32 GetFpcr() const {
        return jit_state.GetFpcr();
    }

    void SetFpcr(u32 value) {
        jit_state.SetFpcr(value);
    }

    u32 GetFpsr() const {
        return jit_state.GetFpsr();
    }

    void SetFpsr(u32 value) {
        jit_state.SetFpsr(value);
    }

    u32 GetPstate() const {
        return jit_state.GetPstate();
    }

    void SetPstate(u32 value) {
        jit_state.SetPstate(value);
    }

    void ClearExclusiveState() {
        jit_state.exclusive_state = 0;
    }

    bool IsExecuting() const {
        return is_executing;
    }

    std::string Disassemble() const {
        const size_t size = reinterpret_cast<const char*>(block_of_code.getCurr()) - reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        auto const* p = reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        return Common::DisassembleX64(p, p + size);
    }

private:
    enum : u32 { EdenOutside = 0, EdenRunning = 1, EdenCompiling = 2, EdenLookup = 3 };

    // Called by the dispatcher of whichever region the core is executing; thisptr is the running
    // core's Impl (loaded from its JIT state, see EdenRunCodeCallbacks).
    static CodePtr GetCurrentBlockThunk(void* thisptr) {
        Jit::Impl* this_ = static_cast<Jit::Impl*>(thisptr);
        EdenCount(this_->conf.processor_id, EdenThunks);
        return this_->EdenEnter([this_] { return this_->GetCurrentBlock(); });
    }

    IR::LocationDescriptor GetCurrentLocation() const {
        return IR::LocationDescriptor{jit_state.GetUniqueHash()};
    }

    // Temporary read-only profiling counters for all six MKW guest cores.
    static inline std::array<std::atomic<u64>, 24> mkw_jit_ns{};
    static inline std::array<std::atomic<u64>, 24> mkw_jit_calls{};
    struct MkwJitTimer {
        unsigned slot;
        u64 start = EdenClockNs();
        ~MkwJitTimer() {
            if (slot < mkw_jit_ns.size()) {
                mkw_jit_ns[slot].fetch_add(EdenClockNs() - start, std::memory_order_relaxed);
                mkw_jit_calls[slot].fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    CodePtr GetCurrentBlock() {
        if (conf.counters) {
            ++conf.counters->block_lookups;
        }
        return GetBlock(GetCurrentLocation());
    }

    CodePtr GetCurrentSingleStep() {
        return GetBlock(A64::LocationDescriptor{GetCurrentLocation()}.SetSingleStepping(true));
    }

    // Looks up (or compiles) the code to enter while no clear can reset it, then marks the core
    // Running; repeats if a clear started or finished in between.
    template<typename Lookup>
    CodePtr EdenEnter(Lookup&& lookup) {
        MkwJitTimer mkw_timer{unsigned(conf.processor_id)};
        for (;;) {
            eden_state.store(EdenLookup, std::memory_order_seq_cst);
            EdenWaitWhileClearing();
            EdenFlushIfRequested();
            const u64 epoch = group->epoch.load(std::memory_order_seq_cst);
            const CodePtr code_ptr = lookup();
            eden_state.store(EdenRunning, std::memory_order_seq_cst);
            if (!group->clearing.load(std::memory_order_seq_cst) &&
                group->epoch.load(std::memory_order_seq_cst) == epoch)
                return code_ptr;
        }
    }

    CodePtr GetBlock(IR::LocationDescriptor descriptor) {
        constexpr size_t MINIMUM_REMAINING_CODESIZE = 1 * 1024 * 1024;
        for (;;) {
            EdenWaitWhileClearing();
            const u64 epoch = group->epoch.load(std::memory_order_seq_cst);
            EdenCount(conf.processor_id, EdenLookups);
            {
                std::unique_lock lock{group->mutex, std::try_to_lock};
                if (!lock.owns_lock()) {
                    EdenCount(conf.processor_id, EdenContended);
                    lock.lock();
                }
                if (const auto* block = group->Find(descriptor)) {
                    EdenCount(conf.processor_id, EdenHits);
                    return block->entrypoint;
                }
            }
            if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {
                EdenReportPressure();
                EdenClearGroup();
                continue;
            }
            // Held until the block is published: a precompilation uses the same emitter.
            eden_compile_waiters.fetch_add(1, std::memory_order_seq_cst);
            std::lock_guard compiling{eden_compile_mutex};
            eden_compile_waiters.fetch_sub(1, std::memory_order_seq_cst);
            block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);

            const u32 previous = eden_state.exchange(EdenCompiling, std::memory_order_seq_cst);
            if (group->clearing.load(std::memory_order_seq_cst)) {
                eden_state.store(previous, std::memory_order_seq_cst);
                continue;
            }
            const u64 start = EdenClockNs();
            const auto get_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };
            // LocationDescriptor ctor() does important ops (like tflags) do not skip
            auto const arch_descriptor = A64::LocationDescriptor{descriptor};
            ir_block.Reset(arch_descriptor);
            A64::Translate(ir_block, arch_descriptor, get_code, {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct});
            const u64 translated = EdenClockNs();
            Optimization::Optimize(ir_block, conf, polyfill_options);
            const u64 optimized = EdenClockNs();
            emitter.eden_pending_sites.clear();
            const auto emitted = emitter.Emit(ir_block, false);
            mkw_native_block(descriptor.Value(), emitted.entrypoint, emitted.size);
            eden_state.store(previous, std::memory_order_seq_cst);
            EdenReportCompile(descriptor, start, translated, optimized);
            eden_space.store(block_of_code.SpaceRemaining(), std::memory_order_relaxed);

            MkwJitTimer mkw_registration{12 + unsigned(conf.processor_id)};
            std::lock_guard lock{group->mutex};
            if (group->epoch.load(std::memory_order_seq_cst) != epoch)
                continue;  // every region was reset after the emission: compile again
            JitGroup::Block registered{};
            group->Register(descriptor, {emitted.entrypoint, emitted.size}, emitter.eden_range_first,
                            emitter.eden_range_last, emitter.eden_pending_sites, registered);
            return registered.entrypoint;
        }
    }

    // Compiles `descriptor` into this core's region from a host thread that is no guest core.
    // 1: compiled and published. 0: already published, or a clear is under way. -2: the region is
    // at its reserve: half of it stays for the core's own compilations, so that a session which
    // goes somewhere the list does not cover is no closer to a clear than it would be with a
    // region half the size. -3: the core is compiling here.
    int EdenPrecompile(IR::LocationDescriptor descriptor) {
        const size_t RESERVE = conf.code_cache_size / 2;
        if (group->clearing.load(std::memory_order_seq_cst))
            return 0;
        u64 invalidations = 0;
        {
            std::lock_guard lock{group->mutex};
            if (group->Find(descriptor))
                return 0;
            invalidations = group->invalidations;
        }
        if (eden_compile_waiters.load(std::memory_order_seq_cst) != 0)
            return -3;
        std::unique_lock compiling{eden_compile_mutex, std::try_to_lock};
        if (!compiling.owns_lock() || eden_compile_waiters.load(std::memory_order_seq_cst) != 0)
            return -3;
        if (block_of_code.SpaceRemaining() < RESERVE)
            return -2;
        const u64 epoch = group->epoch.load(std::memory_order_seq_cst);
        eden_pre_state.store(EdenCompiling, std::memory_order_seq_cst);
        if (group->clearing.load(std::memory_order_seq_cst)) {
            eden_pre_state.store(EdenOutside, std::memory_order_seq_cst);
            return 0;
        }
        block_of_code.EnsureMemoryCommitted(1 * 1024 * 1024);
        const auto get_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };
        auto const arch_descriptor = A64::LocationDescriptor{descriptor};
        ir_block.Reset(arch_descriptor);
        A64::Translate(ir_block, arch_descriptor, get_code, {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct});
        Optimization::Optimize(ir_block, conf, polyfill_options);
        emitter.eden_pending_sites.clear();
        const auto emitted = emitter.Emit(ir_block, false);
        mkw_native_block(descriptor.Value(), emitted.entrypoint, emitted.size);
        eden_pre_state.store(EdenOutside, std::memory_order_seq_cst);
        eden_space.store(block_of_code.SpaceRemaining(), std::memory_order_relaxed);

        std::lock_guard lock{group->mutex};
        if (group->epoch.load(std::memory_order_seq_cst) != epoch)
            return 0;  // every region was reset after the emission
        if (group->invalidations != invalidations)
            return 0;  // the game changed code meanwhile: this block may be of the old code
        JitGroup::Block registered{};
        return group->Register(descriptor, {emitted.entrypoint, emitted.size}, emitter.eden_range_first,
                               emitter.eden_range_last, emitter.eden_pending_sites, registered) ? 1 : 0;
    }

    static int EdenPrecompileThunk(void* member, u64 location) {
        return static_cast<Impl*>(member)->EdenPrecompile(IR::LocationDescriptor{location});
    }

    // Room left in the member's region as of its last compilation (no lock: it is only a hint
    // for choosing the region with the most room).
    static std::size_t EdenSpaceThunk(void* member) {
        return static_cast<Impl*>(member)->eden_space.load(std::memory_order_relaxed);
    }

    void PerformRequestedCacheInvalidation(HaltReason hr) {
        if (Has(hr, HaltReason::CacheInvalidation)) {
            bool entire = false;
            bool quiet = false;
            boost::icl::interval_set<u64> ranges;
            std::vector<u64> demoted;
            {
                std::unique_lock lock{invalidation_mutex};
                demoted.swap(eden_demoted_locations);
                ClearHalt(HaltReason::CacheInvalidation);
                entire = invalidate_entire_cache;
                quiet = eden_quiet_invalidation;
                eden_quiet_invalidation = false;
                invalidate_entire_cache = false;
                ranges.swap(invalid_cache_ranges);
            }
            if (entire) {
                EdenClearGroup();
            } else if (!ranges.empty() || !demoted.empty()) {
                std::vector<u64> invalidated;
                std::vector<Impl*> members;
                {
                    std::lock_guard lock{group->mutex};
                    invalidated = group->Invalidate(ranges);
                    for (const u64 location : demoted)
                        for (const u64 gone : group->InvalidateLocation(IR::LocationDescriptor{location}))
                            invalidated.push_back(gone);
                    if (invalidated.empty())
                        eden_empty_invalidations.fetch_add(1, std::memory_order_relaxed);
                    if (!invalidated.empty())
                        for (void* member : group->members)
                            members.push_back(static_cast<Impl*>(member));
                }
                for (Impl* member : members) {
                    member->EdenQueueFlush(invalidated);
                    if (member != this && !quiet)
                        member->HaltExecution(HaltReason::CacheInvalidation);
                }
            }
        }
        EdenFlushIfRequested();
    }

    // Queues invalidated locations for this core's fast dispatch table (only its own thread may
    // write it: generated code reads entries without synchronization).
    void EdenQueueFlush(const std::vector<u64>& locations) {
        std::lock_guard lock{eden_flush_mutex};
        if (eden_flush_all || eden_flush_locations.size() + locations.size() > 4096) {
            eden_flush_all = true;
            eden_flush_locations.clear();
        } else {
            eden_flush_locations.insert(eden_flush_locations.end(), locations.begin(), locations.end());
        }
        eden_flush.store(true, std::memory_order_seq_cst);
    }

    // On this core's thread before it runs again: drops its return stack buffer and the fast
    // dispatch entries of invalidated blocks (the whole table after many invalidations).
    void EdenFlushIfRequested() {
        if (!eden_flush.exchange(false, std::memory_order_seq_cst))
            return;
        std::vector<u64> locations;
        bool all = false;
        {
            std::lock_guard lock{eden_flush_mutex};
            locations.swap(eden_flush_locations);
            all = std::exchange(eden_flush_all, false);
        }
        std::lock_guard lock{group->mutex};  // a full clear resets these tables too
        jit_state.ResetRSB();
        EdenCount(conf.processor_id, all ? EdenFlushAll : EdenFlushLocations, all ? 1 : locations.size());
        if (all) {
            emitter.ClearFastDispatchTable();
        } else {
            for (const u64 location : locations)
                emitter.ClearFastDispatchEntry(location);
        }
    }

    void EdenWaitWhileClearing() {
        if (!group->clearing.load(std::memory_order_seq_cst))
            return;
        std::unique_lock lock{group->park_mutex};
        group->parked.wait(lock, [this] { return !group->clearing.load(std::memory_order_seq_cst); });
    }

    // Resets every region of the group; called outside generated code (Outside or Lookup).
    void EdenClearGroup() {
        MkwJitTimer mkw_timer{6 + unsigned(conf.processor_id)};
        bool expected = false;
        if (!group->clearing.compare_exchange_strong(expected, true, std::memory_order_seq_cst)) {
            EdenWaitWhileClearing();  // another member is clearing everything
            return;
        }
        std::vector<Impl*> members;
        {
            std::lock_guard lock{group->mutex};
            for (void* member : group->members)
                members.push_back(static_cast<Impl*>(member));
        }
        for (Impl* member : members)
            if (member != this)
                member->HaltExecution(HaltReason::CacheInvalidation);
        for (Impl* member : members) {
            if (member == this)
                continue;
            for (;;) {
                const u32 state = member->eden_state.load(std::memory_order_seq_cst);
                if (state != EdenRunning && state != EdenCompiling)
                    break;
                std::this_thread::yield();
            }
        }
        // A precompilation may be emitting into any member's region, this core's included.
        for (Impl* member : members)
            while (member->eden_pre_state.load(std::memory_order_seq_cst) == EdenCompiling)
                std::this_thread::yield();
        {
            std::lock_guard lock{group->mutex};
            // Eden asks every core's JIT for the same clear or invalidation: this clear satisfies
            // whatever the members had requested so far.
            for (Impl* member : members) {
                std::unique_lock requests{member->invalidation_mutex};
                member->invalidate_entire_cache = false;
                member->invalid_cache_ranges.clear();
            }
            for (Impl* member : members) {
                member->block_of_code.ClearCache();
                member->emitter.ClearCache();
                member->eden_space.store(member->block_of_code.SpaceRemaining(), std::memory_order_relaxed);
                member->jit_state.ResetRSB();
                std::lock_guard flush{member->eden_flush_mutex};
                member->eden_flush_locations.clear();
                member->eden_flush_all = false;
                member->eden_flush.store(false, std::memory_order_seq_cst);
            }
            group->Clear();
            group->epoch.fetch_add(1, std::memory_order_seq_cst);
        }
        {
            std::lock_guard lock{group->park_mutex};
            group->clearing.store(false, std::memory_order_seq_cst);
        }
        group->parked.notify_all();
    }

    // What generated code reads from the JIT state instead of embedding (a64_jitstate.h).
    void EdenInitState() {
        jit_state.eden_callbacks = conf.callbacks;
        jit_state.eden_jit = this;
        jit_state.eden_tpidr_el0 = conf.tpidr_el0;
        jit_state.eden_tpidrro_el0 = conf.tpidrro_el0;
        jit_state.eden_exclusive_address = conf.global_monitor ?
            GetExclusiveMonitorAddressPointer(conf.global_monitor, conf.processor_id) : nullptr;
        jit_state.eden_exclusive_value = conf.global_monitor ?
            GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id) : nullptr;
        jit_state.eden_conf = &conf;
        jit_state.eden_fast_dispatch = emitter.fast_dispatch_table.data();
    }

    void EdenReportPressure() {
        static thread_local unsigned pressure_count;
        if (++pressure_count <= 200)
            std::printf("EDEN_JIT_PRESSURE core=%u capacity=%u remaining=%zu count=%u mono_ns=%lld\n",
                        unsigned(conf.processor_id), unsigned(conf.code_cache_size), block_of_code.SpaceRemaining(),
                        pressure_count, static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch()).count()));
        if (eden_jit_evacuation)
            eden_jit_evacuation(unsigned(conf.processor_id));
    }

    // Compile timers. std::chrono::steady_clock is a system call on the PS5 (about 0.9 µs; four of
    // them per compiled block were 13-15% of compile time), so the port's clock reads the invariant
    // TSC instead; without it (host tools) the steady clock remains.
    static u64 EdenClockNs() {
        if (eden_jit_clock_ns)
            return eden_jit_clock_ns();
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void EdenReportCompile(IR::LocationDescriptor descriptor, u64 start, u64 translated, u64 optimized) {
        const u64 now = EdenClockNs();
        if (eden_jit_compile)
            eden_jit_compile(unsigned(conf.processor_id), now - start);
        if (eden_jit_phases)
            eden_jit_phases(unsigned(conf.processor_id), translated - start, optimized - translated,
                            now - optimized, descriptor.Value());
    }

    IR::Block ir_block = {LocationDescriptor(0, FP::FPCR(0), false)};
    const UserConfig conf;
    A64JitState jit_state;
    BlockOfCode block_of_code;
    A64EmitX64 emitter;
    Optimization::PolyfillOptions polyfill_options;
    bool is_executing = false;
    bool invalidate_entire_cache = false;
    boost::icl::interval_set<u64> invalid_cache_ranges;
    // Only fastmem demotions are pending (EdenInvalidateRunning): the other members are not halted.
    bool eden_quiet_invalidation = false;
    std::vector<u64> eden_demoted_locations;  // blocks of demoted fastmem sites, under invalidation_mutex
    std::mutex invalidation_mutex;
    std::shared_ptr<JitGroup> group;
    std::atomic<u32> eden_state{EdenOutside};
    std::mutex eden_compile_mutex;
    std::atomic<u32> eden_compile_waiters{0};
    std::atomic<u32> eden_pre_state{EdenOutside};
    std::atomic<std::size_t> eden_space{0};
    std::atomic<bool> eden_flush{false};
    std::mutex eden_flush_mutex;
    std::vector<u64> eden_flush_locations;
    bool eden_flush_all = false;
};


extern "C" unsigned long long eden_jit_empty_invalidations() {
    return eden_empty_invalidations.load(std::memory_order_relaxed);
}

extern "C" bool eden_jit_invalidate_running(unsigned long long pc, unsigned long long location) {
    return eden_invalidate_running_hook && eden_invalidate_running_hook(pc, location);
}

Jit::Jit(UserConfig conf)
        : impl(std::make_unique<Jit::Impl>(this, conf)) {}

Jit::~Jit() = default;

HaltReason Jit::Run() {
    return impl->Run();
}

HaltReason Jit::Step() {
    return impl->Step();
}

void Jit::ClearCache() {
    impl->ClearCache();
}

void Jit::InvalidateCacheRange(u64 start_address, size_t length) {
    impl->InvalidateCacheRange(start_address, length);
}

void Jit::Reset() {
    impl->Reset();
}

void Jit::HaltExecution(HaltReason hr) {
    impl->HaltExecution(hr);
}

void Jit::ClearHalt(HaltReason hr) {
    impl->ClearHalt(hr);
}

u64 Jit::GetSP() const {
    return impl->GetSP();
}

void Jit::SetSP(u64 value) {
    impl->SetSP(value);
}

u64 Jit::GetPC() const {
    return impl->GetPC();
}

void Jit::SetPC(u64 value) {
    impl->SetPC(value);
}

u64 Jit::GetRegister(size_t index) const {
    return impl->GetRegister(index);
}

void Jit::SetRegister(size_t index, u64 value) {
    impl->SetRegister(index, value);
}

std::array<u64, 31> Jit::GetRegisters() const {
    return impl->GetRegisters();
}

void Jit::SetRegisters(const std::array<u64, 31>& value) {
    impl->SetRegisters(value);
}

Vector Jit::GetVector(size_t index) const {
    return impl->GetVector(index);
}

void Jit::SetVector(size_t index, Vector value) {
    impl->SetVector(index, value);
}

std::array<Vector, 32> Jit::GetVectors() const {
    return impl->GetVectors();
}

void Jit::SetVectors(const std::array<Vector, 32>& value) {
    impl->SetVectors(value);
}

u32 Jit::GetFpcr() const {
    return impl->GetFpcr();
}

void Jit::SetFpcr(u32 value) {
    impl->SetFpcr(value);
}

u32 Jit::GetFpsr() const {
    return impl->GetFpsr();
}

void Jit::SetFpsr(u32 value) {
    impl->SetFpsr(value);
}

u32 Jit::GetPstate() const {
    return impl->GetPstate();
}

void Jit::SetPstate(u32 value) {
    impl->SetPstate(value);
}

void Jit::ClearExclusiveState() {
    impl->ClearExclusiveState();
}

bool Jit::IsExecuting() const {
    return impl->IsExecuting();
}

std::string Jit::Disassemble() const {
    return impl->Disassemble();
}

}  // namespace Dynarmic::A64
