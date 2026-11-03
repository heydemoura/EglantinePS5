// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Eden::DeviceAllocationProbe {
struct Values {
    std::uint64_t allocations{}, frees{}, allocated_bytes{}, freed_bytes{}, live_bytes{},
                  peak_bytes{}, max_allocation{}, max_since_probe{}, anomalies{};
    std::array<std::uint64_t, 7> buckets{};
};
class Recorder {
    static void Maximum(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
        auto old = target.load(std::memory_order_relaxed);
        while (old < value && !target.compare_exchange_weak(old, value, std::memory_order_relaxed)) {}
    }
public:
    void Event(unsigned type, std::uint64_t size, bool allocated) noexcept {
        if (type >= rows.size()) { invalid_type.fetch_add(1, std::memory_order_relaxed); return; }
        auto& row = rows[type];
        if (allocated) {
            row.allocations.fetch_add(1, std::memory_order_relaxed);
            row.allocated_bytes.fetch_add(size, std::memory_order_relaxed);
            const auto current = row.live_bytes.fetch_add(size, std::memory_order_relaxed) + size;
            Maximum(row.peak_bytes, current);
            Maximum(row.max_allocation, size);
            Maximum(row.max_since_probe, size);
            constexpr std::array<std::uint64_t, 6> bounds{
                16ULL << 20, 64ULL << 20, 128ULL << 20, 256ULL << 20, 512ULL << 20, 1024ULL << 20};
            unsigned bucket{};
            while (bucket < bounds.size() && size > bounds[bucket]) ++bucket;
            row.buckets[bucket].fetch_add(1, std::memory_order_relaxed);
        } else {
            row.frees.fetch_add(1, std::memory_order_relaxed);
            row.freed_bytes.fetch_add(size, std::memory_order_relaxed);
            auto old = row.live_bytes.load(std::memory_order_relaxed);
            do {
                if (old < size) { row.anomalies.fetch_add(1, std::memory_order_relaxed); return; }
            } while (!row.live_bytes.compare_exchange_weak(old, old - size, std::memory_order_relaxed));
        }
    }
    Values Read(unsigned type) noexcept {
        if (type >= rows.size()) { Values v; v.anomalies = 1; return v; }
        auto& row = rows[type];
        Values v{row.allocations.load(), row.frees.load(), row.allocated_bytes.load(), row.freed_bytes.load(),
                 row.live_bytes.load(), row.peak_bytes.load(), row.max_allocation.load(),
                 row.max_since_probe.exchange(0), row.anomalies.load()};
        for (unsigned i = 0; i < v.buckets.size(); ++i) v.buckets[i] = row.buckets[i].load();
        return v; // Individual atomic counters, not a transaction with allocator activity.
    }
    std::uint64_t InvalidType() const { return invalid_type.load(); }
private:
    struct Row {
        std::atomic<std::uint64_t> allocations{}, frees{}, allocated_bytes{}, freed_bytes{}, live_bytes{},
                                   peak_bytes{}, max_allocation{}, max_since_probe{}, anomalies{};
        std::array<std::atomic<std::uint64_t>, 7> buckets{};
    };
    std::array<Row, 32> rows{};
    std::atomic<std::uint64_t> invalid_type{};
};
} // namespace Eden::DeviceAllocationProbe
