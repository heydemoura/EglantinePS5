# The fastmem fault callback without code writes (emit_x64_memory.cpp.inc). Backported from
# ProsperoEden (GPL-3.0-or-later) for the 64-bit fastmem window (src/host_memory.cpp, Window64),
# whose accesses fault by design until the window shows the guest's memory.
#
# Upstream's callback marks the faulting site do-not-fastmem and calls InvalidateBasicBlocks, which
# unpatches the block's links: it writes code from the fault handler, outside the code buffer's
# write window (block_of_code.cpp EnableWriting asserted, then the heap was corrupt). Here the
# callback writes nothing: it records the site in a set every emitter of the unit consults (with the
# shared JIT, one core runs another core's code, and the site must not be compiled onto the window
# again by the next core), and asks for the block's invalidation, which the running core performs
# at its next Run boundary, group-wide (headless/dynarmic/jit_impl.inc). The site table is read and
# written under one lock: another core may fault in this emitter's code while its own core compiles.

function(eden_fastmem_callback_require source anchor what)
    string(FIND "${source}" "${anchor}" at)
    if(at LESS 0)
        message(FATAL_ERROR "Pinned fastmem callback changed (${what})")
    endif()
endfunction()

# Operates on memory_source (emit_x64_memory.cpp.inc), after the port's other changes to it.
macro(eden_fastmem_callback_memory_inc)
    set(eden_fmc_should [=[    const auto marker = std::make_tuple(ctx.Location(), inst->GetName());
    if (do_not_fastmem.count(marker) > 0) {
        return std::nullopt;
    }
    return marker;
}]=])
    set(eden_fmc_add "void AxxEmitX64::AddFastmemPatch(CodePtr location, CodePtr resume_location, void (*callback)(), const DoNotFastmemMarker& marker, bool recompile) {\n")
    set(eden_fmc_callback [=[    if (iter != fastmem_patch_info.end() && iter->rip == u32(rip_ - base)) {
        FakeCall result{
            .call_rip = base + iter->callback,
            .ret_rip = base + iter->resume_rip,
        };
        if (iter->recompile) {
            const DoNotFastmemMarker marker{iter->location, static_cast<unsigned>(iter->inst_name)};
            do_not_fastmem.insert(marker);
            InvalidateBasicBlocks({std::get<0>(marker)});
        }
        return result;
    }]=])
    set(eden_fmc_callback_head "FakeCall AxxEmitX64::FastmemCallback(u64 rip_) {\n")
    foreach(anchor IN ITEMS eden_fmc_should eden_fmc_add eden_fmc_callback eden_fmc_callback_head)
        eden_fastmem_callback_require("${memory_source}" "${${anchor}}" "${anchor}")
    endforeach()
    string(REPLACE "${eden_fmc_should}" [=[    const auto marker = std::make_tuple(ctx.Location(), inst->GetName());
#ifdef EDEN_FASTMEM_A64_UNIT
    std::lock_guard eden_lock{EdenFastmemSiteMutex()};
    if (EdenFastmemDemoted().count({IR::LocationDescriptor{std::get<0>(marker)}.Value(), static_cast<u32>(std::get<1>(marker))}) > 0) {
        return std::nullopt;
    }
#endif
    if (do_not_fastmem.count(marker) > 0) {
        return std::nullopt;
    }
    return marker;
}]=] memory_source "${memory_source}")
    string(REPLACE "${eden_fmc_add}" "${eden_fmc_add}#ifdef EDEN_FASTMEM_A64_UNIT\n    std::lock_guard eden_lock{EdenFastmemSiteMutex()};\n#endif\n" memory_source "${memory_source}")
    string(REPLACE "${eden_fmc_callback_head}" "${eden_fmc_callback_head}#ifdef EDEN_FASTMEM_A64_UNIT\n    std::unique_lock eden_lock{EdenFastmemSiteMutex()};\n#endif\n" memory_source "${memory_source}")
    string(REPLACE "${eden_fmc_callback}" [=[    if (iter != fastmem_patch_info.end() && iter->rip == u32(rip_ - base)) {
        FakeCall result{
            .call_rip = base + iter->callback,
            .ret_rip = base + iter->resume_rip,
        };
        if (iter->recompile) {
#ifndef EDEN_FASTMEM_A64_UNIT
            // A32 (its checked fastmem window, which faults only on races): upstream's handling.
            const DoNotFastmemMarker marker{iter->location, static_cast<unsigned>(iter->inst_name)};
            do_not_fastmem.insert(marker);
            InvalidateBasicBlocks({std::get<0>(marker)});
#else
            // No code is written here (headless/dynarmic/fastmem-callback.cmake).
            const DoNotFastmemMarker marker{iter->location, static_cast<unsigned>(iter->inst_name)};
            do_not_fastmem.insert(marker);
            EdenFastmemDemoted().insert({IR::LocationDescriptor{std::get<0>(marker)}.Value(), static_cast<u32>(std::get<1>(marker))});
            eden_lock.unlock();
            eden_fastmem_demotions.fetch_add(1, std::memory_order_relaxed);
            const u64 pc = EdenFastmemSitePC(jit_interface, std::get<0>(marker));
            if (!eden_jit_invalidate_running || !eden_jit_invalidate_running(pc))
                jit_interface->InvalidateCacheRange(pc, 4);
#endif
        }
        return result;
    }]=] memory_source "${memory_source}")
    # Included inside namespace Dynarmic::Backend::X64 (a32/a64_emit_x64_memory.cpp); the headers
    # are added to those units (eden_fastmem_callback_memory_unit).
    string(PREPEND memory_source [=[// The fastmem fault callback without code writes (headless/dynarmic/fastmem-callback.cmake),
// in the A64 unit (EDEN_FASTMEM_A64_UNIT): the A32 unit keeps upstream's code.
#ifdef EDEN_FASTMEM_A64_UNIT
extern "C" bool eden_jit_invalidate_running(unsigned long long pc) __attribute__((weak));  // jit_impl.inc
inline std::atomic<unsigned long long> eden_fastmem_demotions{0};
namespace {
std::mutex& EdenFastmemSiteMutex() {
    static std::mutex mutex;
    return mutex;
}
// Sites a fault took off fastmem, on any core: (block location, IR instruction index).
std::set<std::pair<u64, u32>>& EdenFastmemDemoted() {
    static std::set<std::pair<u64, u32>> demoted;
    return demoted;
}
[[maybe_unused]] u64 EdenFastmemSitePC(A64::Jit*, IR::LocationDescriptor location) {
    return A64::LocationDescriptor{location}.PC();
}
[[maybe_unused]] u64 EdenFastmemSitePC(A32::Jit*, IR::LocationDescriptor location) {
    return A32::LocationDescriptor{location}.PC();
}
}  // namespace

// For the page-reuse pass (headless/dynarmic/page_reuse_pass.inc): whether a site is off the window.
extern "C" bool eden_fastmem64_site_demoted(unsigned long long location, unsigned inst) {
    std::lock_guard lock{EdenFastmemSiteMutex()};
    return EdenFastmemDemoted().count({location, static_cast<u32>(inst)}) > 0;
}

// The demoted sites of a title, kept across sessions (headless/main.cpp): a later session compiles
// them off the window from the start instead of learning them again through faults. Saved relative
// to the game's code, which loads elsewhere each session, with the game's build ID; another build's
// list is ignored, and so is an older format's list learned under other window rules (EF64: before
// stacks were given pages that agree with the window, full of sites only their misalignment
// demoted; EF65: before pieces with an unmapped page were shown, full of sites only those
// demoted). Bump the format whenever the window's rules change. File: "EF66", the build ID (32 bytes), then per site its location less the code
// start (8 bytes) and its IR instruction index (4 bytes).
extern "C" std::size_t eden_fastmem64_load_sites(const char* path, unsigned long long code_start,
                                                 const unsigned char* build) {
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return 0;
    char magic[4];
    unsigned char saved[32];
    std::size_t loaded = 0;
    if (std::fread(magic, 1, 4, file) == 4 && std::memcmp(magic, "EF66", 4) == 0 &&
        std::fread(saved, 1, 32, file) == 32 && std::memcmp(saved, build, 32) == 0) {
        std::lock_guard lock{EdenFastmemSiteMutex()};
        u64 offset;
        u32 inst;
        while (std::fread(&offset, sizeof(offset), 1, file) == 1 && std::fread(&inst, sizeof(inst), 1, file) == 1)
            loaded += EdenFastmemDemoted().insert({offset + code_start, inst}).second;
    }
    std::fclose(file);
    return loaded;
}
extern "C" std::size_t eden_fastmem64_save_sites(const char* path, unsigned long long code_start,
                                                 unsigned long long code_size, const unsigned char* build) {
    const std::string fresh = std::string{path} + ".new";
    std::FILE* file = std::fopen(fresh.c_str(), "wb");
    if (!file) return 0;
    std::size_t written = 0;
    bool ok = std::fwrite("EF66", 1, 4, file) == 4 && std::fwrite(build, 1, 32, file) == 32;
    {
        std::lock_guard lock{EdenFastmemSiteMutex()};
        for (const auto& [location, inst] : EdenFastmemDemoted()) {
            const u64 pc = A64::LocationDescriptor{IR::LocationDescriptor{location}}.PC();
            if (!ok || pc < code_start || pc - code_start >= code_size) continue;
            const u64 offset = location - code_start;
            ok = std::fwrite(&offset, sizeof(offset), 1, file) == 1 && std::fwrite(&inst, sizeof(inst), 1, file) == 1;
            written += ok;
        }
    }
    ok = std::fclose(file) == 0 && ok && std::rename(fresh.c_str(), path) == 0;
    return ok ? written : 0;
}
#endif
]=])
endmacro()

# Operates on memory_source (a32/a64_emit_x64_memory.cpp): what the callback above needs.
macro(eden_fastmem_callback_memory_unit bits)
    if(${bits} EQUAL 64)
        string(PREPEND memory_source "#define EDEN_FASTMEM_A64_UNIT 1\n")
    endif()
    string(PREPEND memory_source [=[#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <set>
#include <utility>
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A64/a64.h"
]=])
endmacro()

# Operates on eden_impl (headless/dynarmic/jit_impl.inc, A64): which Impl runs on this thread, for
# the callback above.
macro(eden_fastmem_callback_jit_impl)
    set(eden_fmc_impl_head "struct Jit::Impl final {\npublic:\n")
    set(eden_fmc_ctor "        emitter.eden_group = group.get();\n")
    set(eden_fmc_run "        const HaltReason hr = block_of_code.RunCode(&jit_state, current_code_ptr);\n")
    set(eden_fmc_step "        const HaltReason hr = block_of_code.StepCode(&jit_state, code_ptr);\n")
    set(eden_fmc_ranges "    boost::icl::interval_set<u64> invalid_cache_ranges;\n")
    set(eden_fmc_take "                entire = invalidate_entire_cache;\n")
    set(eden_fmc_halt "                    if (member != this)\n                        member->HaltExecution(HaltReason::CacheInvalidation);\n")
    set(eden_fmc_range_add "        invalid_cache_ranges.add(range);\n")
    foreach(anchor IN ITEMS eden_fmc_impl_head eden_fmc_ctor eden_fmc_run eden_fmc_step eden_fmc_ranges eden_fmc_take eden_fmc_halt eden_fmc_range_add)
        eden_fastmem_callback_require("${eden_impl}" "${${anchor}}" "jit_impl.inc ${anchor}")
    endforeach()
    string(REPLACE "${eden_fmc_impl_head}" [=[// The Impl running guest code on this thread, for the fastmem fault callback
// (headless/dynarmic/fastmem-callback.cmake). A function pointer, as the callback cannot name Impl.
static thread_local void* eden_running_impl = nullptr;
static bool (*eden_invalidate_running_hook)(u64) = nullptr;
struct Jit::Impl final {
public:
    // A fastmem site demoted on this thread: its block is invalidated like a code change, at this
    // core's next Run boundary.
    // The guest code did not change, so the other cores are not halted for it: they drop the block
    // at their next Run boundary, and one still inside it faults again and takes the fallback.
    // Halting every core for each of a first session's demotions made it slower than no window.
    static bool EdenInvalidateRunning(u64 pc) {
        auto* const impl = static_cast<Impl*>(eden_running_impl);
        if (!impl) return false;
        {
            std::unique_lock lock{impl->invalidation_mutex};
            impl->invalid_cache_ranges.add(boost::icl::discrete_interval<u64>::closed(pc, pc + 3));
            impl->eden_quiet_invalidation = true;
        }
        impl->HaltExecution(HaltReason::CacheInvalidation);
        return true;
    }
]=] eden_impl "${eden_impl}")
    string(REPLACE "${eden_fmc_ranges}" "${eden_fmc_ranges}    // Only fastmem demotions are pending (EdenInvalidateRunning): the other members are not halted.\n    bool eden_quiet_invalidation = false;\n" eden_impl "${eden_impl}")
    # Any other range invalidation (a code change) halts the members as before.
    string(REPLACE "${eden_fmc_range_add}" "${eden_fmc_range_add}        eden_quiet_invalidation = false;\n" eden_impl "${eden_impl}")
    string(REPLACE "${eden_fmc_take}" "${eden_fmc_take}                quiet = eden_quiet_invalidation;\n                eden_quiet_invalidation = false;\n" eden_impl "${eden_impl}")
    string(REPLACE "            bool entire = false;\n" "            bool entire = false;\n            bool quiet = false;\n" eden_impl "${eden_impl}")
    string(REPLACE "${eden_fmc_halt}" "                    if (member != this && !quiet)\n                        member->HaltExecution(HaltReason::CacheInvalidation);\n" eden_impl "${eden_impl}")
    string(REPLACE "${eden_fmc_ctor}" "${eden_fmc_ctor}        eden_invalidate_running_hook = &EdenInvalidateRunning;\n" eden_impl "${eden_impl}")
    string(REPLACE "${eden_fmc_run}" "        eden_running_impl = this;\n${eden_fmc_run}        eden_running_impl = nullptr;\n" eden_impl "${eden_impl}")
    string(REPLACE "${eden_fmc_step}" "        eden_running_impl = this;\n${eden_fmc_step}        eden_running_impl = nullptr;\n" eden_impl "${eden_impl}")
    string(APPEND eden_impl [=[

extern "C" bool eden_jit_invalidate_running(unsigned long long pc) {
    return eden_invalidate_running_hook && eden_invalidate_running_hook(pc);
}
]=])
endmacro()
