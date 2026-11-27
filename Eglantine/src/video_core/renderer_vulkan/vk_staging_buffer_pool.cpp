// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/common_types.h"
#include "common/literals.h"
#include "video_core/nx2_stutter_trace.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include <atomic>
#include <cstdio>
#include "performance.h"
#include "staging_buffers.h"

namespace Vulkan {
namespace {

using namespace Common::Literals;

// Maximum potential alignment of a Vulkan buffer
constexpr VkDeviceSize MAX_ALIGNMENT = 256;

// Stream buffer size in bytes
// *NIX drivers are more sensitive to increased buffers for streaming.
// Windows ones however, can intake bigger buffers and generally do not OOM.
// - GTX 960 on Windows will not OOM with 256mib
// - GT 1030 on ^NIX will OOM with 256mib
#if defined(__FreeBSD__)
constexpr VkDeviceSize MAX_STREAM_BUFFER_SIZE = 128_MiB;
#else
// 128 MiB on Windows too: 256 MiB sat mostly unused in device memory on Switch 2 titles.
constexpr VkDeviceSize MAX_STREAM_BUFFER_SIZE = 128_MiB;
#endif

// Unused space after the end of the stream buffer. Constant buffers are bound from it and the
// shaders declare each one at its full 64 KiB, so Metal checks 64 KiB from the binding offset
// against the end of the buffer ("argument c3[0] ... has space for 26624 bytes, but argument has a
// length(65536)"). The tail is never handed out, so every binding has the full 64 KiB behind it.
constexpr VkDeviceSize STREAM_BUFFER_TAIL = 64_KiB;

size_t GetStreamBufferSize(const Device& device) {
    if (!device.HasDebuggingToolAttached()) {
        if (device.IsIntegrated()) {
            // An integrated GPU's device-local host-visible heap can be no bigger than the stream
            // buffer (Radeon 780M: 256 MiB, 243 MiB budget), which then sat over that budget.
            // Leave a quarter of it free.
            VkDeviceSize heap_size = 0;
            ForEachDeviceLocalHostVisibleHeap(
                device, [&heap_size](size_t, VkMemoryHeap& heap) {
                    heap_size = (std::max)(heap_size, heap.size);
                });
            if (heap_size != 0 && heap_size <= MAX_STREAM_BUFFER_SIZE) {
                return Common::AlignUp(heap_size * 3 / 4, MAX_ALIGNMENT);
            }
        }
        return MAX_STREAM_BUFFER_SIZE;
    }

    VkDeviceSize size{0};
    bool has_device_local_host_visible_heap{};
    ForEachDeviceLocalHostVisibleHeap(device, [&size, &has_device_local_host_visible_heap](
                                                  size_t index, VkMemoryHeap& heap) {
        has_device_local_host_visible_heap = true;
        size = (std::max)(size, heap.size);
    });
    if (has_device_local_host_visible_heap) {
        // If rebar is not supported, cut the max heap size to 40%. This will allow 2 captures to be
        // loaded at the same time in RenderDoc. If rebar is supported, this shouldn't be an issue
        // as the heap will be much larger.
        if (size <= MAX_STREAM_BUFFER_SIZE) {
            size = size * 40 / 100;
        }
    } else {
        size = MAX_STREAM_BUFFER_SIZE;
    }
    return (std::min)(Common::AlignUp(size, MAX_ALIGNMENT), MAX_STREAM_BUFFER_SIZE);
}
} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Device& device_, MemoryAllocator& memory_allocator_,
                                     Scheduler& scheduler_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      stream_buffer_size{GetStreamBufferSize(device)}, region_size{stream_buffer_size /
                                                                   StagingBufferPool::NUM_SYNCS} {
    VkBufferCreateInfo stream_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = stream_buffer_size + STREAM_BUFFER_TAIL,
        // Transfer destination too: GPU-written list words are copied over their placeholders
        // in the constant buffers streamed here (Nx2ListSlots::PatchUniform).
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        stream_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        stream_ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    stream_buffer = memory_allocator.CreateBuffer(stream_ci, MemoryUsage::Stream);
    if (device.HasDebuggingToolAttached()) {
        stream_buffer.SetObjectNameEXT("Stream Buffer");
    }
    if (device.IsBufferDeviceAddressSupported()) {
        stream_buffer_address = device.GetLogical().GetBufferDeviceAddress(*stream_buffer);
    }
    stream_pointer = stream_buffer.Mapped();
    ASSERT_MSG(!stream_pointer.empty(), "Stream buffer must be host visible!");
}

StagingBufferPool::~StagingBufferPool() = default;

// Its address, next to a caller's in the large-request line below, places the caller in the build's
// ELF: caller - anchor + the anchor's ELF address.
[[gnu::noinline]] static void StagingRequestAnchor() {}

StagingBufferRef StagingBufferPool::Request(size_t size, MemoryUsage usage, bool deferred) {
    // Requests of 64 MiB and more need that much contiguous memory in the shared pool: name the
    // caller (OoT on a standard PS5: a 288 MiB download was refused, then the session crashed).
    if (size >= (size_t{64} << 20)) {
        static std::atomic<unsigned> large{0};
        if (large.fetch_add(1, std::memory_order_relaxed) < 64) {
            std::fprintf(stderr, "EDEN_STAGING_LARGE size=%zuMiB usage=%u deferred=%d caller=%p anchor=%p\n",
                         size >> 20, unsigned(usage), int(deferred), __builtin_return_address(0),
                         reinterpret_cast<void*>(&StagingRequestAnchor));
        }
    }
    if (!deferred && usage == MemoryUsage::Upload && size <= region_size) {
        return GetStreamBuffer(size);
    }
    return GetStagingBuffer(size, usage, deferred);
}

void StagingBufferPool::FreeDeferred(StagingBufferRef& ref) {
    auto& entries = GetCache(ref.usage)[ref.log2_level].entries;
    const auto is_this_one = [&ref](const StagingBuffer& entry) {
        return entry.index == ref.index;
    };
    auto it = std::find_if(entries.begin(), entries.end(), is_this_one);
    ASSERT(it != entries.end());
    ASSERT(it->deferred);
    it->tick = scheduler.CurrentTick();
    it->deferred = false;
}

void StagingBufferPool::ReleaseCompleted() {
    const auto idle = [this](const StagingBuffer& entry) {
        return !entry.deferred && scheduler.IsFree(entry.tick);
    };
    for (StagingBuffersCache* const cache : {&device_local_cache, &upload_cache, &download_cache}) {
        for (StagingBuffers& level : *cache) {
            ::Eden::StagingBuffers::ReleaseFrom(level, idle);
        }
    }
}

void StagingBufferPool::TickFrame() {
    current_delete_level = (current_delete_level + 1) % NUM_LEVELS;

    ReleaseCache(MemoryUsage::DeviceLocal);
    ReleaseCache(MemoryUsage::Upload);
    ReleaseCache(MemoryUsage::Download);
    if (::Eden::Performance::GraphicsMemoryIsShort()) {
        ReleaseCompleted();
        return;
    }
    // Large buffers (over 32 MiB) go as soon as the GPU is done with them. The console's one pool is
    // shared with the game's memory: kept for 64 frames, idle ones held 0.5-0.9 GB in Ocarina of
    // Time (uploads up to 288 MiB). Making one again costs less than the pool running out.
    constexpr size_t KeepMaxLog2 = 25;
    const auto idle = [this](const StagingBuffer& entry) {
        return !entry.deferred && scheduler.IsFree(entry.tick);
    };
    for (StagingBuffersCache* const cache : {&device_local_cache, &upload_cache, &download_cache}) {
        for (size_t level = KeepMaxLog2 + 1; level < NUM_LEVELS; ++level) {
            if (!(*cache)[level].entries.empty()) {
                ::Eden::StagingBuffers::ReleaseFrom((*cache)[level], idle);
            }
        }
    }
}

StagingBufferRef StagingBufferPool::GetStreamBuffer(size_t size) {
    if (AreRegionsActive(Region(free_iterator) + 1,
                         (std::min)(Region(iterator + size) + 1, NUM_SYNCS))) {
        // Avoid waiting for the previous usages to be free
        return GetStagingBuffer(size, MemoryUsage::Upload);
    }
    const u64 current_tick = scheduler.CurrentTick();
    std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + Region(iterator),
              current_tick);
    used_iterator = iterator;
    free_iterator = (std::max)(free_iterator, iterator + size);

    if (iterator + size >= stream_buffer_size) {
        std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + NUM_SYNCS,
                  current_tick);
        used_iterator = 0;
        iterator = 0;
        free_iterator = size;

        if (AreRegionsActive(0, Region(size) + 1)) {
            // Avoid waiting for the previous usages to be free
            return GetStagingBuffer(size, MemoryUsage::Upload);
        }
    }
    const size_t offset = iterator;
    iterator = Common::AlignUp(iterator + size, MAX_ALIGNMENT);
    return StagingBufferRef{
        .buffer = *stream_buffer,
        .device_address = stream_buffer_address,
        .offset = static_cast<VkDeviceSize>(offset),
        .mapped_span = stream_pointer.subspan(offset, size),
        .usage{},
        .log2_level{},
        .index{},
    };
}

bool StagingBufferPool::AreRegionsActive(size_t region_begin, size_t region_end) const {
    const u64 gpu_tick = scheduler.GetMasterSemaphore().KnownGpuTick();
    return std::any_of(sync_ticks.begin() + region_begin, sync_ticks.begin() + region_end,
                       [gpu_tick](u64 sync_tick) { return gpu_tick < sync_tick; });
};

StagingBufferRef StagingBufferPool::GetStagingBuffer(size_t size, MemoryUsage usage,
                                                     bool deferred) {
    if (const std::optional<StagingBufferRef> ref = TryGetReservedBuffer(size, usage, deferred)) {
        return *ref;
    }
    return CreateStagingBuffer(size, usage, deferred);
}

std::optional<StagingBufferRef> StagingBufferPool::TryGetReservedBuffer(size_t size,
                                                                        MemoryUsage usage,
                                                                        bool deferred) {
    StagingBuffers& cache_level = GetCache(usage)[Common::Log2Ceil(size)];

    const auto is_free = [this, size](const StagingBuffer& entry) {
        return !entry.deferred && scheduler.IsFree(entry.tick) &&
               ::Eden::StagingBuffers::Holds(entry.mapped_span.size(), entry.log2_level) >= size;
    };
    auto& entries = cache_level.entries;
    const auto hint_it = entries.begin() + cache_level.iterate_index;
    auto it = std::find_if(entries.begin() + cache_level.iterate_index, entries.end(), is_free);
    if (it == entries.end()) {
        it = std::find_if(entries.begin(), hint_it, is_free);
        if (it == hint_it) {
            return std::nullopt;
        }
    }
    cache_level.iterate_index = std::distance(entries.begin(), it) + 1;
    it->tick = deferred ? (std::numeric_limits<u64>::max)() : scheduler.CurrentTick();
    ASSERT(!it->deferred);
    it->deferred = deferred;
    return it->Ref();
}

StagingBufferRef StagingBufferPool::CreateStagingBuffer(size_t size, MemoryUsage usage, bool deferred) {
    if (static unsigned rows{}; size >= (64ULL << 20) && rows++ < 96) {
        for (unsigned kind = 0; kind < 3; ++kind) {
            auto& cache = kind == 0 ? device_local_cache : kind == 1 ? upload_cache : download_cache;
            size_t count = 0, pinned = 0, busy = 0, free = 0;
            unsigned long long bytes = 0;
            for (const auto& level : cache) for (const auto& entry : level.entries) {
                ++count;
                bytes += ::Eden::StagingBuffers::Holds(entry.mapped_span.size(), entry.log2_level);
                if (entry.deferred) ++pinned;
                else if (scheduler.IsFree(entry.tick)) ++free;
                else ++busy;
            }
            std::fprintf(stderr, "MKW_STAGING_MISS asked=%zu usage=%u deferred=%d kind=%u count=%zu bytes=%llu pinned=%zu busy=%zu free=%zu tick=%llu completed=%llu\n",
                         size, unsigned(usage), int(deferred), kind, count, bytes, pinned, busy, free,
                         static_cast<unsigned long long>(scheduler.CurrentTick()),
                         static_cast<unsigned long long>(scheduler.GetMasterSemaphore().KnownGpuTick()));
        }
    }

    const VideoCommon::StutterTrace::SiteScope site{VideoCommon::StutterTrace::Site::StagingCreate};
    auto const log2_size = Common::Log2Ceil<u32>(u32(size));
    const bool exact = ::Eden::StagingBuffers::Exact(size, usage != MemoryUsage::DeviceLocal);
    const VkDeviceSize made = ::Eden::StagingBuffers::Size(size, log2_size, exact);
    if (exact) {
        // Kept buffers of this power of two too small for the transfer go before it is made.
        ::Eden::StagingBuffers::ReleaseFrom(GetCache(usage)[log2_size], [this, size](const StagingBuffer& entry) {
            return !entry.deferred && scheduler.IsFree(entry.tick) &&
                   ::Eden::StagingBuffers::Holds(entry.mapped_span.size(), entry.log2_level) < size;
        });
    }
    VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = made,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, // mesh emulation dispatch and draw arguments
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        buffer_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        buffer_ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    vk::Buffer buffer;
    try {
        buffer = memory_allocator.CreateBuffer(buffer_ci, usage);
    } catch (const vk::Exception& refused) {
        if (refused.GetResult() != VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            throw;
        }
        // Refused: the kept buffers the GPU is done with go, and the buffer is asked for once
        // more. Refused again, the error goes on as before.
        const auto idle = [this](const StagingBuffer& entry) {
            return !entry.deferred && scheduler.IsFree(entry.tick);
        };
        ::Eden::StagingBuffers::Released released;
        for (StagingBuffersCache* const cache : {&device_local_cache, &upload_cache, &download_cache}) {
            for (StagingBuffers& level : *cache) {
                released += ::Eden::StagingBuffers::ReleaseFrom(level, idle);
            }
        }
        std::fprintf(stderr, "EDEN_STAGING_OOM asked=%zu made=%llu released=%zu buffers (%llu MiB)\n", size,
                     static_cast<unsigned long long>(made), released.buffers,
                     static_cast<unsigned long long>(released.bytes >> 20));
        buffer = memory_allocator.CreateBuffer(buffer_ci, usage);
    }
    // A large buffer smaller than the power of two: the 1st, 2nd, 4th, 8th ... in the log.
    if (exact && made >= (VkDeviceSize{64} << 20) && made < (VkDeviceSize{1} << log2_size)) {
        static std::atomic<unsigned long long> large{0};
        const auto times = large.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((times & (times - 1)) == 0) {
            std::fprintf(stderr, "EDEN_STAGING_EXACT %llu MiB made for a transfer of %llu MiB, not the %llu MiB of the "
                         "next power of two (%llu so far)\n",
                         static_cast<unsigned long long>(made >> 20), static_cast<unsigned long long>(size >> 20),
                         static_cast<unsigned long long>((VkDeviceSize{1} << log2_size) >> 20), times);
        }
    }
    if (device.HasDebuggingToolAttached()) {
        ++buffer_index;
        buffer.SetObjectNameEXT(fmt::format("Staging Buffer {}", buffer_index).c_str());
    }
    const std::span<u8> mapped_span = buffer.Mapped();
    const VkDeviceAddress buffer_address =
        device.IsBufferDeviceAddressSupported()
            ? device.GetLogical().GetBufferDeviceAddress(*buffer)
            : VkDeviceAddress{};
    StagingBuffer& entry = GetCache(usage)[log2_size].entries.emplace_back(StagingBuffer{
        .buffer = std::move(buffer),
        .device_address = buffer_address,
        .mapped_span = mapped_span,
        .usage = usage,
        .log2_level = log2_size,
        .index = unique_ids++,
        .tick = deferred ? (std::numeric_limits<u64>::max)() : scheduler.CurrentTick(),
        .deferred = deferred,
    });
    return entry.Ref();
}

StagingBufferPool::StagingBuffersCache& StagingBufferPool::GetCache(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
        return device_local_cache;
    case MemoryUsage::Upload:
        return upload_cache;
    case MemoryUsage::Download:
        return download_cache;
    default:
        ASSERT_MSG(false, "Invalid memory usage={}", usage);
        return upload_cache;
    }
}

u64 StagingBufferPool::CachedBytes(MemoryUsage usage) {
    u64 bytes{};
    const StagingBuffersCache& cache = GetCache(usage);
    for (size_t level = 0; level < NUM_LEVELS; ++level) {
        if (!cache[level].entries.empty()) {
            bytes += u64{cache[level].entries.size()} << level;
        }
    }
    return bytes;
}

void StagingBufferPool::ReleaseCache(MemoryUsage usage) {
    ReleaseLevel(GetCache(usage), current_delete_level);
}

void StagingBufferPool::ReleaseLevel(StagingBuffersCache& cache, size_t log2) {
    constexpr size_t deletions_per_tick = 16;
    auto& staging = cache[log2];
    auto& entries = staging.entries;
    const size_t old_size = entries.size();

    // Keep two idle upload buffers per level up to 32 MiB (under 128 MiB in all). Each level is
    // swept every 64 frames, so after a pause each upload over 8 MiB (past the stream buffer's
    // regions) got a new dedicated allocation, written to for the first time (16 MiB on an
    // RTX 4080, idle system: 0.4 ms to create, 1.2 ms more for the first write).
    constexpr size_t KeepMaxLog2 = 25;
    constexpr size_t KeepPerLevel = 2;
    if (&cache == &upload_cache && log2 <= KeepMaxLog2) {
        size_t idle = 0;
        std::erase_if(entries, [this, &idle](const StagingBuffer& entry) {
            return scheduler.IsFree(entry.tick) && ++idle > KeepPerLevel;
        });
        staging.delete_index = 0;
        if (staging.iterate_index > entries.size()) {
            staging.iterate_index = 0;
        }
        return;
    }

    const auto is_deletable = [this](const StagingBuffer& entry) {
        return scheduler.IsFree(entry.tick);
    };
    const size_t begin_offset = staging.delete_index;
    const size_t end_offset = (std::min)(begin_offset + deletions_per_tick, old_size);
    const auto begin = entries.begin() + begin_offset;
    const auto end = entries.begin() + end_offset;
    entries.erase(std::remove_if(begin, end, is_deletable), end);

    const size_t new_size = entries.size();
    staging.delete_index += deletions_per_tick;
    if (staging.delete_index >= new_size) {
        staging.delete_index = 0;
    }
    if (staging.iterate_index > new_size) {
        staging.iterate_index = 0;
    }
}

} // namespace Vulkan
