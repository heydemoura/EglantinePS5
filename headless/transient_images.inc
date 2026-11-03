// Included in namespace Vulkan by the native allocator adaptation.
// Only explicit temporary-image callers use these pools. GPU completion still
// controls image destruction through the existing pending-transcode queue.
vk::Image MemoryAllocator::CreateTemporaryImage(const VkImageCreateInfo& ci) const {
    const char* option = std::getenv("EDEN_PS5_TRANSIENT_POOLS");
    if (!option || std::strcmp(option, "1") != 0) return CreateImage(ci);
    constexpr VkDeviceSize block_size = 16ULL << 20;
    VkImage handle{};
    auto& dispatch = device.GetDispatchLoader();
    const auto logical = *device.GetLogical();
    if (dispatch.vkCreateImage(logical, &ci, nullptr, &handle) != VK_SUCCESS)
        return CreateImage(ci);
    VkMemoryRequirements requirements{};
    dispatch.vkGetImageMemoryRequirements(logical, handle, &requirements);
    VmaAllocationCreateInfo request{};
    request.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    // Generic type selection has no image-usage context; AUTO is invalid for
    // vmaFindMemoryTypeIndex. These images require no CPU mapping.
    request.usage = VMA_MEMORY_USAGE_UNKNOWN;
    request.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    u32 type{};
    VmaAllocation allocation{};
    bool bound = false;
    if (requirements.size <= block_size &&
        vmaFindMemoryTypeIndex(allocator, requirements.memoryTypeBits & valid_memory_types,
                               &request, &type) == VK_SUCCESS && type < temporary_pools.size()) {
        std::lock_guard lock{temporary_pool_mutex};
        auto& pool = temporary_pools[type];
        if (!pool) {
            VmaPoolCreateInfo pool_info{};
            pool_info.memoryTypeIndex = type;
            pool_info.blockSize = block_size;
            pool_info.minBlockCount = 0;
            pool_info.maxBlockCount = 32; // at most 512 MiB for each eligible memory type
            const auto result = vmaCreatePool(allocator, &pool_info, &pool);
            if (result != VK_SUCCESS) pool = nullptr;
            if (pool) vmaSetPoolName(allocator, pool, "PS5 temporary decode images");
        }
        if (pool) {
            request.pool = pool;
            VmaAllocationInfo info{};
            if (vmaAllocateMemoryForImage(allocator, handle, &request, &allocation, &info) == VK_SUCCESS)
                bound = vmaBindImageMemory(allocator, allocation, handle) == VK_SUCCESS;
        }
    }
    if (bound) {
        static std::atomic<unsigned> hits{};
        const auto count = hits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 512 == 0)
            std::fprintf(stderr, "MKW_TRANSIENT_POOL used=%u type=%u bytes=%llu\n", count, type,
                         static_cast<unsigned long long>(requirements.size));
        return vk::Image(handle, ci.usage, logical, allocator, allocation, dispatch);
    }
    // Nothing has been recorded against this image. Release partial resources
    // before the ordinary allocator handles large images or a refused pool.
    if (allocation) vmaFreeMemory(allocator, allocation);
    dispatch.vkDestroyImage(logical, handle, nullptr);
    return CreateImage(ci);
}
