namespace {
// Development GPU execution time (dev_vulkan.h gpu_time): timestamps at the start and end
// of each upload+graphics submission, summed once the GPU has run it and printed every
// 5 s as EDEN_GPU_TIME. Probe/counters are worker-thread-only. Origin is captured on
// the recording thread, then copied into the submission command.
using ::Eden::VulkanProbe::GpuSubmitOrigin;
using ::Eden::VulkanProbe::gpu_submit_origin;
using ::Eden::VulkanProbe::GpuSubmitOriginScope;
struct GpuTimeProbe {
    static constexpr u32 SLOTS = 512;
    vk::QueryPool pool;
    PFN_vkCmdWriteTimestamp write{};
    double period_ns{};
    u32 valid_bits{};
    u64 next{}, first_pending{};
    u32 current{};
    u64 busy_ns{}, max_ns{}, submissions{}, total_ns{}, total_submissions{};
    std::chrono::steady_clock::time_point report{};
    bool failed{};
    struct OriginCount { GpuSubmitOrigin origin; u64 count{}; };
    std::array<OriginCount, 64> origins{};
    u32 origin_slots{};
    u64 origin_overflow{};
} gpu_time;

void GpuTimeCountOrigin(GpuSubmitOrigin origin) {
    if (!::Eden::DevVulkan::gpu_time || gpu_time.failed) return;
    for (u32 i = 0; i < gpu_time.origin_slots; ++i) {
        auto& row = gpu_time.origins[i];
        if (row.origin.caller == origin.caller && row.origin.finish == origin.finish) {
            ++row.count;
            return;
        }
    }
    if (gpu_time.origin_slots == gpu_time.origins.size()) { ++gpu_time.origin_overflow; return; }
    gpu_time.origins[gpu_time.origin_slots++] = {origin, 1};
}

void GpuTimeBegin(const Device& device, vk::CommandBuffer cmdbuf) {
    if (!::Eden::DevVulkan::gpu_time || gpu_time.failed) return;
    if (!gpu_time.write) {
        const auto& dld = device.GetDispatchLoader();
        gpu_time.write = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
            dld.vkGetDeviceProcAddr(*device.GetLogical(), "vkCmdWriteTimestamp"));
        gpu_time.period_ns = device.GetPhysical().GetProperties().limits.timestampPeriod;
        const auto families = device.GetPhysical().GetQueueFamilyProperties();
        if (device.GetGraphicsFamily() < families.size())
            gpu_time.valid_bits = families[device.GetGraphicsFamily()].timestampValidBits;
        if (!gpu_time.write || gpu_time.period_ns <= 0 || !gpu_time.valid_bits || gpu_time.valid_bits > 64) { gpu_time.failed = true; return; }
        gpu_time.pool = device.GetLogical().CreateQueryPool({
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = GpuTimeProbe::SLOTS * 2,
            .pipelineStatistics = 0,
        });
        gpu_time.report = std::chrono::steady_clock::now();
        std::printf("EDEN_GPU_TIME_INIT period_ns=%.9f valid_bits=%u slots=%u\n",
                    gpu_time.period_ns, gpu_time.valid_bits, GpuTimeProbe::SLOTS);
        std::printf("EDEN_GPU_SUBMIT_ANCHOR address=%p coverage=upload+graphics\n",
                    reinterpret_cast<void*>(&GpuTimeCountOrigin));
    }
    if (gpu_time.next - gpu_time.first_pending >= GpuTimeProbe::SLOTS) {
        gpu_time.failed = true; // results stopped arriving; never overwrite a pending slot
        std::printf("EDEN_GPU_TIME failed=ring\n");
        return;
    }
    gpu_time.current = static_cast<u32>(gpu_time.next++ % GpuTimeProbe::SLOTS);
    cmdbuf.ResetQueryPool(*gpu_time.pool, gpu_time.current * 2, 2);
    gpu_time.write(*cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *gpu_time.pool, gpu_time.current * 2);
}

void GpuTimeEnd(vk::CommandBuffer cmdbuf) {
    if (!::Eden::DevVulkan::gpu_time || gpu_time.failed || !gpu_time.write) return;
    gpu_time.write(*cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *gpu_time.pool, gpu_time.current * 2 + 1);
}

void GpuTimeCollect(const Device& device) {
    if (!::Eden::DevVulkan::gpu_time || gpu_time.failed || !gpu_time.write) return;
    while (gpu_time.first_pending + 1 < gpu_time.next) {
        const u32 slot = static_cast<u32>(gpu_time.first_pending % GpuTimeProbe::SLOTS);
        std::array<u64, 2> stamps{};
        if (device.GetLogical().GetQueryResults(*gpu_time.pool, slot * 2, 2, sizeof(stamps), stamps.data(),
                                               sizeof(u64), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) break;
        const u64 mask = gpu_time.valid_bits == 64 ? ~u64{0} : (u64{1} << gpu_time.valid_bits) - 1;
        const u64 ns = static_cast<u64>(double((stamps[1] - stamps[0]) & mask) * gpu_time.period_ns);
        gpu_time.busy_ns += ns;
        gpu_time.total_ns += ns;
        ++gpu_time.total_submissions;
        gpu_time.max_ns = (std::max)(gpu_time.max_ns, ns);
        ++gpu_time.submissions;
        ++gpu_time.first_pending;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(now - gpu_time.report).count();
    if (wall >= 5'000'000'000) {
        std::printf("EDEN_GPU_TIME wall_ms=%.1f busy_ms=%.1f submissions=%llu max_ms=%.2f cumulative_ns=%llu cumulative_submissions=%llu pending=%llu\n", wall / 1e6,
                    gpu_time.busy_ns / 1e6, static_cast<unsigned long long>(gpu_time.submissions),
                    gpu_time.max_ns / 1e6, static_cast<unsigned long long>(gpu_time.total_ns),
                    static_cast<unsigned long long>(gpu_time.total_submissions),
                    static_cast<unsigned long long>(gpu_time.next - gpu_time.first_pending));
        gpu_time.busy_ns = gpu_time.max_ns = gpu_time.submissions = 0;
        u64 origin_total = gpu_time.origin_overflow;
        for (u32 i = 0; i < gpu_time.origin_slots; ++i) {
            auto& row = gpu_time.origins[i];
            origin_total += row.count;
            if (row.count) std::printf("EDEN_GPU_SUBMIT_ORIGIN caller=%p finish=%d count=%llu\n",
                row.origin.caller, int(row.origin.finish), static_cast<unsigned long long>(row.count));
            row.count = 0;
        }
        std::printf("EDEN_GPU_SUBMIT_TOTAL count=%llu overflow=%llu\n",
                    static_cast<unsigned long long>(origin_total),
                    static_cast<unsigned long long>(gpu_time.origin_overflow));
        gpu_time.origin_overflow = 0;
        gpu_time.report = now;
    }
}
void GpuTimeReset(const Device& device) {
    if (gpu_time.pool) {
        const auto result = device.GetDispatchLoader().vkDeviceWaitIdle(*device.GetLogical());
        if (result != VK_SUCCESS) std::fprintf(stderr, "EDEN_GPU_TIME reset_idle_result=%d\n", int(result));
    }
    // Called only after the scheduler worker joined, while its device is alive.
    gpu_time = {};
}
} // namespace
