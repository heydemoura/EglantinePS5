// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <utility>
#include "dev_vulkan.h"

namespace Eden::VulkanProbe {
struct GpuSubmitOrigin {
    const void* caller{};
    bool finish{};
};
inline thread_local GpuSubmitOrigin gpu_submit_origin;
// Cache wrappers name their caller; Scheduler preserves the outer attribution.
// This does not alter submission or completion behavior.
struct GpuSubmitOriginScope {
    GpuSubmitOrigin saved{};
    bool enabled{};
    GpuSubmitOriginScope(const void* caller, bool finish, bool preserve = false)
        : enabled{DevVulkan::gpu_time && !(preserve && gpu_submit_origin.caller)} {
        if (enabled) saved = std::exchange(gpu_submit_origin, GpuSubmitOrigin{caller, finish});
    }
    ~GpuSubmitOriginScope() { if (enabled) gpu_submit_origin = saved; }
};
} // namespace Eden::VulkanProbe
