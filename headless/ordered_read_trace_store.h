// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "video_core/readback_dependency_diagnostics.h"

namespace Eden::OrderedReadTrace {
using Record = VideoCommon::ReadbackDependencyDiagnostics::Record;
using Area = VideoCore::RasterizerDownloadArea;

// One writer per accepted slot. Capture storage stays alive through draining and export.
// Frame boundaries and request timestamps allow host-side clipping across frame intervals.
template <std::size_t Capacity, std::size_t FrameCapacity>
class Store {
public:
    struct Entry { Record read; Area area{}; };
    struct CopyEntry {
        std::uint64_t ns{}, frame{}, base{}, buffer_size{}, offset{}, size{};
        std::uint64_t sparse_compatible{}, memory_type{}, asynchronous{}, copy_tick{};
        std::uint64_t allocation_id{}, candidate_producer_tick{}, write_version{}, candidate_producer_completed{};
    };
    static constexpr std::size_t CopyCapacity = std::min<std::size_t>(Capacity, 16384);
    struct FetchEntry {
        std::uint64_t begin_ns{}, end_ns{}, start_frame{}, end_frame{};
        std::uint64_t target{}, queued_end{}, consumed_before{}, mode{};
    };
    static constexpr std::uint64_t NoFetch = ~std::uint64_t{0};
    struct Boundary { std::uint64_t frame{}, ns{}, buffer_gpu_bytes{}, buffer_cpu_bytes{}, mapped_direct_bytes{}, mapped_checked_bytes{}, mapped_failures{}, command_snapshot_bytes{}; };
    enum Phase : unsigned { Idle, Recording, Draining, Exporting, Done };

    void Start(std::uint64_t nonce_, std::uint64_t frame, std::uint64_t ns,
               unsigned frame_count, std::uint64_t gpu_bytes = 0, std::uint64_t cpu_bytes = 0, std::uint64_t direct = 0,
               std::uint64_t checked = 0, std::uint64_t failures = 0,
               std::uint64_t snapshot_bytes = 0) {
        nonce = nonce_;
        next.store(0);
        dropped.store(0);
        copy_next.store(0);
        copy_dropped.store(0);
        fetch_next.store(0);
        fetch_dropped.store(0);
        inflight.store(0);
        frames = 1;
        limit = std::clamp<unsigned>(frame_count, 1, FrameCapacity - 1);
        boundaries[0] = {frame, ns, gpu_bytes, cpu_bytes, direct, checked, failures, snapshot_bytes};
        current_frame.store(frame + 1);
        phase.store(Recording, std::memory_order_release);
    }
    bool Begin(Record& read, std::uint64_t ns) {
        if (phase.load(std::memory_order_acquire) != Recording) return false;
        inflight.fetch_add(1, std::memory_order_acq_rel);
        if (phase.load(std::memory_order_acquire) != Recording) {
            inflight.fetch_sub(1, std::memory_order_release);
            return false;
        }
        const auto index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= Capacity) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            inflight.fetch_sub(1, std::memory_order_release);
            return false;
        }
        read.trace_index = index;
        read.request_ns = ns;
        read.start_frame = current_frame.load(std::memory_order_acquire);
        return true;
    }
    void Copy(CopyEntry entry) {
        if (phase.load(std::memory_order_acquire) != Recording) return;
        inflight.fetch_add(1, std::memory_order_acq_rel);
        if (phase.load(std::memory_order_acquire) != Recording) {
            inflight.fetch_sub(1, std::memory_order_release);
            return;
        }
        const auto index = copy_next.fetch_add(1, std::memory_order_relaxed);
        if (index < CopyCapacity) {
            entry.frame = current_frame.load(std::memory_order_acquire);
            copies[index] = entry;
        } else {
            copy_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        inflight.fetch_sub(1, std::memory_order_release);
    }
    std::size_t CopyCount() const { return std::min(copy_next.load(), CopyCapacity); }

    std::uint64_t BeginFetch(std::uint64_t ns, std::uint64_t target,
                             std::uint64_t queued_end, std::uint64_t consumed_before,
                             unsigned mode) {
        if (phase.load(std::memory_order_acquire) != Recording) return NoFetch;
        inflight.fetch_add(1, std::memory_order_acq_rel);
        if (phase.load(std::memory_order_acquire) != Recording) {
            inflight.fetch_sub(1, std::memory_order_release);
            return NoFetch;
        }
        const auto index = fetch_next.fetch_add(1, std::memory_order_relaxed);
        if (index >= Capacity) {
            fetch_dropped.fetch_add(1, std::memory_order_relaxed);
            inflight.fetch_sub(1, std::memory_order_release);
            return NoFetch;
        }
        fetches[index] = {ns, 0, current_frame.load(std::memory_order_acquire), 0,
                          target, queued_end, consumed_before, mode};
        return index;
    }
    void EndFetch(std::uint64_t index, std::uint64_t ns) {
        auto& entry = fetches[index];
        entry.end_ns = ns;
        entry.end_frame = current_frame.load(std::memory_order_acquire);
        inflight.fetch_sub(1, std::memory_order_release);
    }
    std::size_t FetchCount() const { return std::min(fetch_next.load(), Capacity); }

    void End(const Record& read, const Area& area) {
        auto& entry = entries[read.trace_index];
        entry.read = read;
        entry.read.end_frame = current_frame.load(std::memory_order_acquire);
        entry.area = area;
        inflight.fetch_sub(1, std::memory_order_release);
    }
    void Frame(std::uint64_t frame, std::uint64_t ns,
               std::uint64_t gpu_bytes = 0, std::uint64_t cpu_bytes = 0, std::uint64_t direct = 0,
               std::uint64_t checked = 0, std::uint64_t failures = 0,
               std::uint64_t snapshot_bytes = 0) {
        current_frame.store(frame + 1, std::memory_order_release);
        if (phase.load(std::memory_order_acquire) != Recording) return;
        if (frame <= boundaries[frames - 1].frame) return;
        boundaries[frames++] = {frame, ns, gpu_bytes, cpu_bytes, direct, checked, failures, snapshot_bytes};
        if (frames == limit + 1) phase.store(Draining, std::memory_order_release);
    }
    bool Ready() const {
        return phase.load(std::memory_order_acquire) == Draining &&
               inflight.load(std::memory_order_acquire) == 0;
    }
    std::size_t Count() const { return std::min(next.load(), Capacity); }

    std::array<Entry, Capacity> entries;
    std::array<CopyEntry, CopyCapacity> copies;
    std::array<FetchEntry, Capacity> fetches;
    std::atomic<std::uint64_t> fetch_next{}, fetch_dropped{};
    std::atomic<std::uint64_t> copy_next{}, copy_dropped{};
    std::array<Boundary, FrameCapacity> boundaries{};
    std::atomic<unsigned> phase{Idle};
    std::atomic<std::uint64_t> next{}, dropped{}, inflight{}, current_frame{};
    std::uint64_t nonce{};
    unsigned frames{}, limit{};
};
}
