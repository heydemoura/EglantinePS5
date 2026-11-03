// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sys/mman.h>

namespace Eden {
// Keep the full guest-callback stack size. Anonymous CPU mappings use the console's
// flexible pool, leaving direct memory for guest RAM and GPU resources. Guard pages
// surround the usable stack. Exhaustion falls back to the existing heap domain.
class FiberStack {
public:
    FiberStack() = default;
    FiberStack(const FiberStack&) = delete;
    FiberStack& operator=(const FiberStack&) = delete;
    ~FiberStack() {
        if (mapping) {
            if (munmap(mapping, bytes + 2 * GuardBytes)) std::abort();
            flexible_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        } else if (fallback) {
            if (std::memcmp(fallback.get(), Canary.data(), CanaryBytes) ||
                std::memcmp(base + bytes, Canary.data(), CanaryBytes)) std::abort();
            fallback_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        }
    }
    void Allocate(std::size_t size) {
        if (!size || size % GuardBytes || bytes) std::abort();
        bytes = size;
        void* reserved = mmap(nullptr, bytes + 2 * GuardBytes, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reserved != MAP_FAILED) {
            auto* usable = static_cast<unsigned char*>(reserved) + GuardBytes;
            if (mprotect(usable, bytes, PROT_READ | PROT_WRITE) == 0) {
                mapping = reserved;
                base = usable;
                flexible_bytes.fetch_add(bytes, std::memory_order_relaxed);
            } else if (munmap(reserved, bytes + 2 * GuardBytes)) {
                std::abort();
            }
        }
        if (!base) {
            fallback.reset(new unsigned char[bytes + 2 * CanaryBytes]);
            base = fallback.get() + CanaryBytes;
            std::memcpy(fallback.get(), Canary.data(), CanaryBytes);
            std::memcpy(base + bytes, Canary.data(), CanaryBytes);
            fallback_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        const auto count = allocations.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((count & (count - 1)) == 0) {
            std::fprintf(stderr, "EDEN_FIBER_STACK allocations=%llu flexible=%llu fallback=%llu stack_bytes=%zu\n",
                         count, flexible_bytes.load(), fallback_bytes.load(), bytes);
        }
    }
    unsigned char* data() noexcept { return base; }
    std::size_t size() const noexcept { return bytes; }
    inline static std::atomic<unsigned long long> flexible_bytes{}, fallback_bytes{}, allocations{};
    static constexpr std::size_t GuardBytes = 16384;
private:
    inline static constexpr std::array<unsigned long long, 2> Canary{
        0x8d569a12e3c047bfULL, 0x729db60f24e1ac83ULL};
    static constexpr std::size_t CanaryBytes = sizeof(Canary);
    void* mapping{};
    unsigned char* base{};
    std::size_t bytes{};
    std::unique_ptr<unsigned char[]> fallback;
};
} // namespace Eden
