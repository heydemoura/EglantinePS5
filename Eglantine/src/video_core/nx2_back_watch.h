// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>

#include "common/common_types.h"

/// Where the split GPU thread's back half is, for EDEN_NX2_BACK_WATCH=1: a watchdog
/// (GpuSplit::WatchBack) logs the step and the ring entry it has been on for 120 ms or more.
/// A thread blocked on a lock takes no profiler sample (headless/hang_stacks.h); this names it.
namespace VideoCommon::BackWatch {

inline const bool on = std::getenv("EDEN_NX2_BACK_WATCH") != nullptr;
inline std::atomic<const char*> step{"start"};
inline std::atomic<u64> progress{};
inline std::atomic<u64> entry{};
inline std::atomic<u64> entry_next{};

inline void Step(const char* name) {
    step.store(name, std::memory_order_relaxed);
}

} // namespace VideoCommon::BackWatch
