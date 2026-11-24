// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>

#include <atomic>
#include <optional>

#include "common/common_types.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/present/frame_gen_pacer.h"
#include "video_core/renderer_vulkan/present/lsfg_chain.h"
#include "video_core/renderer_vulkan/present/lsfg_shaders.h"
#include "video_core/renderer_vulkan/present/fsr3_frame_gen.h"
#include "video_core/renderer_vulkan/present/mihawk_frame_gen.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

class Device;
class Scheduler;
struct Frame;

class FrameGen {
public:
    explicit FrameGen(MemoryAllocator& memory_allocator, Scheduler& scheduler);
    ~FrameGen();

    void Process(const Device& device, Frame* frame, VkFormat format, VkExtent2D guest_extent);

    [[nodiscard]] size_t WantedGenerations(size_t capacity);

    [[nodiscard]] size_t GeneratedFrameCount() const;

    /// The measured interval between real frames, in seconds (0 before it is known).
    [[nodiscard]] f32 FrameInterval() const {
        return pacer.SmoothedInterval();
    }

    void GenerateInto(const Device& device, Frame* destination, size_t generation);

    [[nodiscard]] VideoCore::FrameGenStatus CurrentStatus() const {
        return {.state = state.load(std::memory_order_relaxed),
                .generated_frames = generated_frames.load(std::memory_order_relaxed)};
    }

private:
    void Rebuild(const Device& device, VkExtent2D extent, VkFormat format, f32 flow_scale);
    void DumpDebugImages(u64 count);

    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;

    std::optional<LsfgShaders> shaders;
    std::optional<LsfgChain> chain;
    std::optional<Fsr3FrameGen> fsr3_chain;
    std::optional<MihawkFrameGen> mihawk_chain;
    FrameGenPacer pacer;
    FrameGenPlan plan{};
    VkExtent2D peak_guest_extent{};
    VkExtent2D built_extent{};
    VkFormat built_format{VK_FORMAT_UNDEFINED};
    f32 built_flow_scale{};
    u64 frame_count{};
    u64 last_count{};
    size_t last_generations{};
    u32 warm_streak{};
    bool generated{};
    bool unavailable{};
    bool dumped{};
    bool generation_reported{};
    bool storage_reported{};
    std::atomic<VideoCore::FrameGenState> state{VideoCore::FrameGenState::Pending};
    std::atomic<u64> generated_frames{};
    std::chrono::steady_clock::time_point report_start{};
    u64 report_generated{};
    u32 report_frames{};
};

} // namespace Vulkan
