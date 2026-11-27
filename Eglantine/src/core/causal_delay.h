// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdlib>

// Causal profiling (development, env.txt): a known delay added to one cost at a time; how much the
// frame rate drops tells how much removing that cost would give.
//   EDEN_CAUSAL_JIT_PCT=N     after each JIT run, wait N% of its length (game code N% slower)
//   EDEN_CAUSAL_SVC_NS=N      N ns more per SVC
//   EDEN_CAUSAL_CACHED_NS=N   N ns more per write to GPU-cached guest memory (guest cores)
//   EDEN_CAUSAL_CACHED_READ_NS=N  the same for reads
namespace Core::Causal {

inline long Read(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::strtol(value, nullptr, 10) : 0;
}
inline long JitPercent() { static const long value = Read("EDEN_CAUSAL_JIT_PCT"); return value; }
inline long SvcNs() { static const long value = Read("EDEN_CAUSAL_SVC_NS"); return value; }
inline long CachedNs() { static const long value = Read("EDEN_CAUSAL_CACHED_NS"); return value; }
inline long CachedReadNs() { static const long value = Read("EDEN_CAUSAL_CACHED_READ_NS"); return value; }

inline void Spin(long long ns) {
    if (ns <= 0) return;
    const auto end = std::chrono::steady_clock::now() + std::chrono::nanoseconds{ns};
    while (std::chrono::steady_clock::now() < end) {
    }
}

} // namespace Core::Causal
