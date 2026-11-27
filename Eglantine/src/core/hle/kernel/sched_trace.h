// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// EDEN_SCHED_TRACE=<start s>:<seconds> (env.txt): every guest thread switch, wake-up (who made a
// waiting thread runnable) and present in that window, with TSC times, written to
// logs/sched-trace.bin for a critical-path walk (24-byte Records; a wake-up also keeps the first
// 6 characters of the waking host thread's name in pad and pad2).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <thread>
#include <vector>
#include <x86intrin.h>
#include "common/common_types.h"
#include "common/thread.h"

namespace Kernel::SchedTrace {

struct Record {
    u64 tsc;
    u32 a, b;      // switch: from, to; wake: waker, wakee; present: thread, 0
    u8 kind, core; // 1 switch, 2 wake, 3 present
    u16 pad;
    u32 pad2;
};
static_assert(sizeof(Record) == 24);

inline constexpr size_t Capacity = 1 << 19;

struct State {
    std::atomic<int> phase{0}; // 0 not started, 1 armed, 2 recording, 3 done
    u64 start_tsc{}, end_tsc{};
    std::atomic<size_t> count{};
    Record* records{};
};
inline State g_state;

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_SCHED_TRACE");
        if (value == nullptr) return false;
        const double start = std::atof(value);
        const char* colon = std::strchr(value, ':');
        const double seconds = colon ? std::atof(colon + 1) : 2.0;
        constexpr double tsc_hz = 1596300000.0; // the console's TSC (core.cpp "Host clock")
        const u64 now = __rdtsc();
        g_state.records = new Record[Capacity];
        g_state.start_tsc = now + u64(start * tsc_hz);
        g_state.end_tsc = g_state.start_tsc + u64(seconds * tsc_hz);
        g_state.phase.store(1);
        return true;
    }();
    return enabled;
}

inline void Dump() {
    std::thread([] {
        const size_t n = (std::min)(g_state.count.load(), Capacity);
        if (std::FILE* file = std::fopen("/data/eglantineps5/logs/sched-trace.bin", "wb")) {
            std::fwrite(g_state.records, sizeof(Record), n, file);
            std::fclose(file);
        }
    }).detach();
}

inline void Add(u8 kind, u8 core, u32 a, u32 b) {
    if (!Enabled()) return;
    const int phase = g_state.phase.load(std::memory_order_relaxed);
    if (phase == 3 || phase == 0) return;
    const u64 now = __rdtsc();
    if (now < g_state.start_tsc) return;
    if (now >= g_state.end_tsc) {
        int expected = 1;
        if (g_state.phase.compare_exchange_strong(expected, 3) ||
            (expected = 2, g_state.phase.compare_exchange_strong(expected, 3))) {
            Dump();
        }
        return;
    }
    if (phase == 1) g_state.phase.store(2, std::memory_order_relaxed);
    const size_t i = g_state.count.fetch_add(1, std::memory_order_relaxed);
    if (i >= Capacity) return;
    Record record{now, a, b, kind, core, 0, 0};
    if (kind == 2) {
        // The host thread that made it runnable: the first 6 characters of its name.
        char name[8]{};
        std::strncpy(name, Common::CurrentThreadName(), 6);
        std::memcpy(&record.pad, name, 2);
        std::memcpy(&record.pad2, name + 2, 4);
    }
    g_state.records[i] = record;
}

} // namespace Kernel::SchedTrace
