// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#ifdef _MSC_VER
#include <intrin.h>
#elif defined(ARCHITECTURE_x86_64)
#include <x86intrin.h>
#endif

#include "common/common_types.h"

// EDEN_NX2_TC_COST=1: what the texture cache's churn costs on the thread that runs it. Each scope
// below adds its wall time (time stamp counter, converted per report) and calls; the texture
// cache logs the totals with its "last 600 frames" lines, next to the window's wall time and the
// thread's CPU time. Off: one look at a flag per scope.
//
// Scopes nest (Delete in Collect, Construct and some Refresh in Insert, Upload in Refresh,
// AstcDispatch in Upload, Finish anywhere): each slot is inclusive. `churn` sums the outermost
// churn scope only (no double counting): collection, destruction, creation, first uploads and
// view creation, with whatever waits they made.
namespace VideoCommon::TcCost {

enum class Slot : u32 {
    TickFrame,     ///< TextureCache::TickFrame as a whole (Collect and Destroy included)
    Collect,       ///< RunGarbageCollector
    Delete,        ///< DeleteImage (unregistering, views, aliases; memory is freed in Destroy)
    Destroy,       ///< Sentenced images, views and framebuffers destroyed (VMA frees, vkDestroy*)
    Insert,        ///< InsertImage: joins, creation and the first upload when made by a join
    Construct,     ///< The Image constructor (vkCreateImage, VMA allocation and bind)
    Allocate,      ///< Of Construct: MakeImage alone (vkCreateImage + VMA alloc + bind)
    ViewCreate,    ///< Image views made (FindOrEmplaceImageView)
    RefreshNew,    ///< RefreshContents of an image made this frame (re-creation's upload)
    RefreshOld,    ///< RefreshContents of an older image (guest wrote it: not churn)
    Upload,        ///< UploadImageContents (guest read, unswizzle/convert, copy recording)
    AstcDispatch,  ///< AccelerateImageUpload: ASTC decode/BCn transcode recording
    Finish,        ///< TextureCacheRuntime::Finish and transcode waits (host GPU idle waits)
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Slot::Count)> slot_names{
    "tick_frame", "collect", "delete",    "destroy",     "insert",      "construct", "allocate",
    "view_create", "refresh_new", "refresh_old", "upload", "astc_dispatch", "finish",
};

inline constexpr std::array<bool, static_cast<size_t>(Slot::Count)> churn_slots{
    false, true, true, true, true, true, true, true, true, false, true, true, false,
};

[[nodiscard]] inline bool ReadEnv() {
    const char* const value = std::getenv("EDEN_NX2_TC_COST");
    return value != nullptr && value[0] == '1';
}

/// Read at the first use (the first frame), after the frontend has applied its switches.
[[nodiscard]] inline bool Enabled() {
    static const bool enabled = ReadEnv();
    return enabled;
}

inline u64 Ticks() {
#ifdef ARCHITECTURE_x86_64
    return __rdtsc();
#else
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
#endif
}

struct Totals {
    std::array<std::atomic<u64>, static_cast<size_t>(Slot::Count)> ticks{};
    std::array<std::atomic<u64>, static_cast<size_t>(Slot::Count)> calls{};
    std::atomic<u64> churn_ticks{};
};

inline Totals g_totals;
/// Open scopes on this thread (TickFrame aside): a churn scope counts in churn_ticks only when
/// no other scope is open, so nested ones and uploads under RefreshOld are not counted twice.
inline thread_local u32 t_depth{};

class Scope {
public:
    explicit Scope(Slot slot_) : slot{slot_}, on{Enabled()} {
        if (!on) {
            return;
        }
        // TickFrame only contains the others: it does not hide them.
        nests = slot != Slot::TickFrame;
        outer = churn_slots[static_cast<size_t>(slot)] && t_depth == 0;
        if (nests) {
            ++t_depth;
        }
        start = Ticks();
    }
    ~Scope() {
        if (!on) {
            return;
        }
        const u64 elapsed = Ticks() - start;
        const auto index = static_cast<size_t>(slot);
        g_totals.ticks[index].fetch_add(elapsed, std::memory_order_relaxed);
        g_totals.calls[index].fetch_add(1, std::memory_order_relaxed);
        if (nests) {
            --t_depth;
        }
        if (outer) {
            g_totals.churn_ticks.fetch_add(elapsed, std::memory_order_relaxed);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Slot slot;
    bool on;
    bool nests{};
    bool outer{};
    u64 start{};
};

} // namespace VideoCommon::TcCost
