// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/present/frame_gen.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "diagnostics.h"

namespace Vulkan {

namespace {

#if defined(__PROSPERO__) // PS5
// The output's refresh period in seconds: 119.88 Hz when the session asked for 120 Hz
// (headless/display_refresh.h), 59.94 Hz otherwise.
f32 RefreshSeconds() {
    const char* high = std::getenv("EDEN_VIDEOOUT_120HZ");
    return high != nullptr && high[0] == '1' ? 1.0f / 119.88f : 1.0f / 59.94f;
}
#endif

constexpr size_t COLOR_CHANNELS = 4;
constexpr u64 LSFG_REQUIRED_FRAMES = 2;
constexpr u32 LSFG_RECURRENCE_FRAMES = 2;

[[nodiscard]] f32 ManualFlowScale() {
    return static_cast<f32>(Settings::values.frame_gen_flow_scale.GetValue()) / 100.0f;
}

[[nodiscard]] f32 ConfiguredFlowScale(VkExtent2D guest_extent, VkExtent2D presented_extent) {
    if (!Settings::values.frame_gen_flow_scale_auto.GetValue()) {
        return ManualFlowScale();
    }
    if (guest_extent.width == 0 || presented_extent.width == 0) {
        return 1.0f;
    }

    const f32 rendered_width = static_cast<f32>(guest_extent.width) *
                               Settings::values.resolution_info.up_factor;
    const f32 ratio = rendered_width / static_cast<f32>(presented_extent.width);

    constexpr f32 FLOW_SCALE_STEPS = 20.0f;
    const f32 stepped = std::ceil(ratio * FLOW_SCALE_STEPS) / FLOW_SCALE_STEPS;
    return std::clamp(stepped, 0.25f, 1.0f);
}

bool IsBlueFirst(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

VkDeviceSize BytesPerTexel(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM:
        return 1;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8;
    default:
        return COLOR_CHANNELS;
    }
}

void WritePortablePixmap(const std::filesystem::path& path, const std::string& magic,
                         VkExtent2D extent, std::span<const u8> pixels) {
    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Write,
                            Common::FS::FileType::BinaryFile};
    if (!file.IsOpen()) {
        return;
    }

    const std::string header = magic + "\n" + std::to_string(extent.width) + " " +
                               std::to_string(extent.height) + "\n255\n";
    if (file.Write(header) != header.size()) {
        return;
    }

    void(file.Write(pixels));
    void(file.Flush());
}

void WriteGrayscalePgm(const std::filesystem::path& path, VkExtent2D extent,
                       std::span<const u8> pixels) {
    const size_t expected = static_cast<size_t>(extent.width) * extent.height;
    WritePortablePixmap(path, "P5", extent, pixels.subspan(0, (std::min)(expected, pixels.size())));
}

void WriteRaw(const std::filesystem::path& path, std::span<const u8> pixels) {
    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Write,
                            Common::FS::FileType::BinaryFile};
    if (!file.IsOpen()) {
        return;
    }
    void(file.Write(pixels));
    void(file.Flush());
}

void WriteColorPpm(const std::filesystem::path& path, VkExtent2D extent,
                   std::span<const u8> pixels, bool blue_first) {
    const size_t pixel_count = static_cast<size_t>(extent.width) * extent.height;
    if (pixels.size() < pixel_count * COLOR_CHANNELS) {
        return;
    }

    std::vector<u8> rgb(pixel_count * 3);
    for (size_t i = 0; i < pixel_count; ++i) {
        const u8 first = pixels[i * COLOR_CHANNELS];
        const u8 green = pixels[i * COLOR_CHANNELS + 1];
        const u8 third = pixels[i * COLOR_CHANNELS + 2];
        rgb[i * 3] = blue_first ? third : first;
        rgb[i * 3 + 1] = green;
        rgb[i * 3 + 2] = blue_first ? first : third;
    }

    WritePortablePixmap(path, "P6", extent, rgb);
}

VkImageMemoryBarrier MakeTransitionBarrier(VkImage image, VkAccessFlags src_access,
                                           VkAccessFlags dst_access, VkImageLayout old_layout,
                                           VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
}

VkImageCopy MakeCopyRegion(VkExtent2D extent) {
    return VkImageCopy{
        .srcSubresource{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcOffset = {},
        .dstSubresource{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .dstOffset = {},
        .extent = {.width = extent.width, .height = extent.height, .depth = 1},
    };
}

void CopyPresentedFrame(vk::CommandBuffer cmdbuf, VkImage source, LsfgImage& destination,
                        VkExtent2D extent) {
    const auto make_barrier = MakeTransitionBarrier;

    const std::array before{
        make_barrier(source, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        make_barrier(destination.Handle(), VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     destination.Layout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);

    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.Handle(),
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, MakeCopyRegion(extent));

    const std::array after{
        make_barrier(source, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        make_barrier(destination.Handle(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, {}, {}, after);

    destination.SetLayout(VK_IMAGE_LAYOUT_GENERAL);
}

} // Anonymous namespace

FrameGen::FrameGen(MemoryAllocator& memory_allocator_, Scheduler& scheduler_)
    : memory_allocator{memory_allocator_}, scheduler{scheduler_} {}

FrameGen::~FrameGen() = default;

void FrameGen::Process(const Device& device, Frame* frame, VkFormat format,
                       VkExtent2D guest_extent) {
    generated = false;

    if (!shaders) {
        shaders.emplace(device);
    }

    if (unavailable || !Settings::values.frame_gen.GetValue()) {
        if (chain || fsr3_chain || mihawk_chain) {
            scheduler.Finish();
            chain.reset();
            fsr3_chain.reset();
            mihawk_chain.reset();
        }
        warm_streak = 0;
        return;
    }

    // Only Lossless Scaling needs the user's shaders (its Lossless.dll).
    if (!shaders->IsValid() && Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::Lsfg) {
        unavailable = true;
        state.store(shaders->IsDeviceSupported() ? VideoCore::FrameGenState::MissingLosslessDll
                                                 : VideoCore::FrameGenState::UnsupportedGpu,
                    std::memory_order_relaxed);
        return;
    }

    if (!frame->storage_view) {
        state.store(VideoCore::FrameGenState::UnsupportedSurface, std::memory_order_relaxed);
        warm_streak = 0;
        return;
    }
    state.store(VideoCore::FrameGenState::Running, std::memory_order_relaxed);

    peak_guest_extent.width = (std::max)(peak_guest_extent.width, guest_extent.width);
    peak_guest_extent.height = (std::max)(peak_guest_extent.height, guest_extent.height);

    const VkExtent2D extent{.width = frame->width, .height = frame->height};
    const f32 flow_scale = ConfiguredFlowScale(peak_guest_extent, extent);
    if ((!chain && !fsr3_chain && !mihawk_chain) || built_extent.width != extent.width || built_extent.height != extent.height ||
        built_format != format || built_flow_scale != flow_scale) {
        Rebuild(device, extent, format, flow_scale);
    }

    const u64 count = frame_count++;
    last_count = count;
    last_generations = plan.generations;

    const bool warm = plan.warm && count + 1 >= LSFG_REQUIRED_FRAMES;
    warm_streak = warm ? warm_streak + 1 : 0;
    generated = warm && warm_streak >= LSFG_RECURRENCE_FRAMES && plan.generations > 0;
#if defined(__PROSPERO__) // PS5
    // Make only the frames the display has a free refresh for. A game slower than the output
    // leaves (interval x rate - 1) refreshes free per real frame (rate: the target, at most the
    // output's refresh): at 43 FPS toward 60, 0.39. The present thread leaves out a generated
    // frame that would hold the game back, so making one per real frame anyway spent the GPU on
    // frames nobody saw (Zelda: Ocarina of Time, 31 of 43 a second left out) and took that time
    // from the game. The credit carries the fraction over, so 0.39 makes a frame every second or
    // third real one. MihawkGen and AMD FSR 3 need only the two latest frames, so this decides
    // alone: the pacer's own rules (no frames while frame times jump about, or for a second
    // after) made almost none in DK Bananza, whose frame times vary all the time. Lossless
    // Scaling keeps the pacer (its chain warms up over frames).
    if (Settings::values.frame_gen_type.GetValue() != Settings::FrameGenType::Lsfg) {
        const f32 interval = pacer.SmoothedInterval();
        const u32 target = Settings::values.frame_gen_target_rate.GetValue();
        const f32 refresh_rate = 1.0f / RefreshSeconds();
        const f32 rate = target > 0 ? (std::min)(static_cast<f32>(target), refresh_rate) : refresh_rate;
        const size_t cap = (std::min)(Settings::FrameGenMaxGenerations(), last_capacity);
        const auto now = std::chrono::steady_clock::now();
        const f32 own_interval =
            last_process == std::chrono::steady_clock::time_point{}
                ? 0.0f
                : std::chrono::duration<f32>(now - last_process).count();
        last_process = now;
        size_t fill = 0;
        if (cap > 0 && interval > 0.0f && count + 1 >= LSFG_REQUIRED_FRAMES) {
            // No more saved up than one frame's share rounds up to: saved-up frames spent at once
            // found no free refresh and were left out (10 a second in Zelda and DK Bananza).
            const f32 share = (std::max)(interval * rate - 1.0f, 0.0f);
            const size_t frame_cap =
                (std::min)(cap, static_cast<size_t>(std::ceil(share)));
            fill_credit = std::clamp(fill_credit + share, 0.0f, static_cast<f32>(frame_cap));
            fill = (std::min)(static_cast<size_t>(fill_credit), frame_cap);
            // Spent only on a frame whose own interval leaves room (half a refresh past the real
            // frames): frame times that swing (DK Bananza: about 20 and 34 ms in turn) have room
            // after the long frames, Zelda: Ocarina of Time's steady 30 ms after each.
            while (fill > 0 && own_interval * rate < static_cast<f32>(fill) + 0.5f) {
                --fill;
            }
            fill_credit -= static_cast<f32>(fill);
        } else {
            fill_credit = 0.0f;
        }
        last_generations = fill;
        generated = fill > 0;
        if (generated && !generation_reported) {
            generation_reported = true;
            Eden::Report("framegen", "generating frames for the refreshes the game leaves free");
        }
    }
#endif
    if (generated && !generation_reported) {
        generation_reported = true;
        Eden::Report("framegen",
                     ("generating " + std::to_string(plan.generations) +
                      " extra frame(s) per guest frame").c_str());
    }
    generated_frames.fetch_add(GeneratedFrameCount(), std::memory_order_relaxed);
    // The rate frames are made at, next to the guest's own (nx2:vi2's "last 600 presents" line):
    // every 600 guest frames, how many frames were generated between them, and in how long.
    {
        const auto now = std::chrono::steady_clock::now();
        if (report_frames == 0) {
            report_start = now;
            report_generated = generated_frames.load(std::memory_order_relaxed);
        }
        if (++report_frames == 600) {
            const f64 seconds = std::chrono::duration<f64>(now - report_start).count();
            const u64 extra = generated_frames.load(std::memory_order_relaxed) - report_generated;
            if (seconds > 0.0) {
                LOG_INFO(Render_Vulkan,
                         "Frame generation: last 600 guest frames in {:.2f} s, {} generated: "
                         "{:.1f} guest/s, {:.1f} made/s (shown: the present thread's line)",
                         seconds, extra, 599.0 / seconds, (599.0 + static_cast<f64>(extra)) / seconds);
            }
            report_frames = 0;
        }
    }

    const bool use_fsr3 = Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::Fsr3;

    if (mihawk_chain) {
        scheduler.RequestOutsideRenderPassOperationContext();
        mihawk_chain->Process(scheduler, *frame->image, count);
    } else if (use_fsr3 && fsr3_chain) {
        scheduler.RequestOutsideRenderPassOperationContext();
        fsr3_chain->Process(device, scheduler, *frame->image, *frame->storage_view, count);
    } else if (chain) {
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([this, source = *frame->image, extent, count,
                          dispatch = warm](vk::CommandBuffer cmdbuf) {
            CopyPresentedFrame(cmdbuf, source, chain->Input(count), extent);
            if (dispatch) {
                chain->DispatchShared(cmdbuf, count);
            }
        });

        const bool dump_requested = generated && Settings::values.frame_gen_dump_flow.GetValue();
        if (!dump_requested) {
            dumped = false;
        } else if (!dumped) {
            DumpDebugImages(count);
            dumped = true;
        }
    }
}

size_t FrameGen::WantedGenerations(size_t capacity) {
    if (unavailable) {
        plan = {};
        return 0;
    }
    plan = pacer.Plan(capacity);
    last_capacity = capacity;
    return plan.generations;
}

size_t FrameGen::GeneratedFrameCount() const {
    return generated ? last_generations : 0;
}

void FrameGen::GenerateInto(const Device& device, Frame* destination, size_t generation) {
    const bool use_fsr3 = Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::Fsr3;

    if (mihawk_chain) {
        const f32 t = static_cast<f32>(generation + 1) / static_cast<f32>(last_generations + 1);
        scheduler.RequestOutsideRenderPassOperationContext();
        mihawk_chain->Generate(scheduler, *destination->image, *destination->storage_view, t);
    } else if (use_fsr3 && fsr3_chain) {
        const f32 blend = static_cast<f32>(generation + 1) / static_cast<f32>(last_generations + 1);
        const f32 flow_scale = ConfiguredFlowScale(peak_guest_extent, built_extent);

        scheduler.RequestOutsideRenderPassOperationContext();
        fsr3_chain->Generate(device, scheduler, *destination->image, *destination->storage_view,
                             blend, flow_scale);
    } else if (chain) {
        chain->SetTarget(device, last_generations, generation, destination->index,
                         *destination->storage_view);

        const VkExtent2D extent{.width = destination->width, .height = destination->height};

        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([this, count = last_count, generation_count = last_generations, generation,
                          target = destination->index, image = *destination->image,
                          extent](vk::CommandBuffer cmdbuf) {
            chain->DispatchGeneration(cmdbuf, count, generation_count, generation, target, image,
                                      extent);
        });
    }
}

void FrameGen::Rebuild(const Device& device, VkExtent2D extent, VkFormat format, f32 flow_scale) {
    scheduler.Finish();
    chain.reset();
    fsr3_chain.reset();
    mihawk_chain.reset();

    built_flow_scale = flow_scale;

    if (Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::Mihawk) {
        mihawk_chain.emplace(device, memory_allocator, extent, format);
        if (!mihawk_chain->IsValid()) {
            mihawk_chain.reset();
            unavailable = true;
            state.store(VideoCore::FrameGenState::UnsupportedGpu, std::memory_order_relaxed);
            Eden::Report("framegen", "MihawkGen could not be set up; see the log");
        }
    } else if (Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::Fsr3) {
        fsr3_chain.emplace(device, memory_allocator, extent, format);
    } else {
        chain.emplace(device, memory_allocator, *shaders, extent, format, built_flow_scale);
    }
    built_extent = extent;
    built_format = format;
    frame_count = 0;
    warm_streak = 0;
    generated = false;
}

void FrameGen::DumpDebugImages(u64 count) {
    const std::filesystem::path directory =
        Common::FS::GetEdenPath(Common::FS::EdenPath::LosslessDir) / "debug";
    if (!Common::FS::CreateDirs(directory)) {
        return;
    }

    const auto dump = [&](const std::string& name, LsfgImage& image) {
        const VkExtent2D extent = image.Extent();
        const VkFormat format = image.Format();
        const VkDeviceSize texel_size = BytesPerTexel(format);
        const VkDeviceSize size =
            static_cast<VkDeviceSize>(extent.width) * extent.height * texel_size;

        vk::Buffer buffer = CreateWrappedBuffer(memory_allocator, size, MemoryUsage::Download);

        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record(
            [handle = image.Handle(), dst = *buffer, extent](vk::CommandBuffer cmdbuf) {
                DownloadColorImage(
                    cmdbuf, handle, dst,
                    VkExtent3D{.width = extent.width, .height = extent.height, .depth = 1});
            });
        scheduler.Finish();

        buffer.Invalidate();
        const std::span<u8> mapped = buffer.Mapped();

        if (format == LSFG_FLOW_FORMAT) {
            WriteGrayscalePgm(directory / (name + ".pgm"), extent, mapped);
        } else if (texel_size == COLOR_CHANNELS) {
            WriteColorPpm(directory / (name + ".ppm"), extent, mapped, IsBlueFirst(format));
        } else {
            WriteRaw(directory / (name + "_" + std::to_string(extent.width) + "x" +
                                  std::to_string(extent.height) + ".f16"),
                     mapped.subspan(0, std::min<size_t>(size, mapped.size())));
        }
    };

    dump("in0", chain->Input(0));
    dump("in1", chain->Input(1));

    for (size_t level = 0; level < LSFG_MIP_LEVELS; ++level) {
        dump("flow_mip" + std::to_string(level), chain->FlowLevel(level));
    }
    for (size_t index = 0; index < 2; ++index) {
        dump("alpha0_" + std::to_string(index), chain->AlphaOutput(0, count, index));
        dump("alpha6_" + std::to_string(index), chain->AlphaOutput(LSFG_MIP_LEVELS - 1, count,
                                                                  index));
    }
    for (size_t level = 0; level < LSFG_BETA_OUTPUTS; ++level) {
        dump("beta_" + std::to_string(level), chain->BetaOutput(level));
    }

    dump("gamma0", chain->GammaOutput(0));
    dump("gamma6", chain->GammaOutput(LSFG_MIP_LEVELS - 1));
    dump("delta2_out1", chain->DeltaOutput1(LSFG_DELTA_INSTANCES - 1));
    dump("delta2_out2", chain->DeltaOutput2(LSFG_DELTA_INSTANCES - 1));
}

} // namespace Vulkan
