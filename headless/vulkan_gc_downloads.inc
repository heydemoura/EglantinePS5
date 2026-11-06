    // Age-eligible images in the emptiest allocator blocks go first (zortos293/Eglantine
    // d1b7f69df7, after official Eglantine c0cb3325; tools/eglantine-patches/0012 had the same
    // order for Cleanup alone, from at most 8 candidates per eviction). Oldest first left every
    // 16 MiB block partly used and new images filled the holes, so the pool got few blocks back
    // and stayed short through races. Up to 1,024 of the oldest candidates are ordered by the share
    // of their block images still hold (counted once a second), ties in age order; past them the
    // walk goes on oldest first (visiting those again). The download lookahead and Cleanup walk
    // the same order, so a batch holds the images Cleanup takes next.
    const auto ForEachCandidate = [&](u64 below, auto&& visit) {
        if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {
            if (block_image_bytes_frame == ~0ULL || frame_tick - block_image_bytes_frame >= 60) {
                block_image_bytes_frame = frame_tick;
                block_image_bytes.clear();
                for (const auto [id, image] : slot_images) {
                    if (const auto block = image->MemoryBlock(); block.block != 0)
                        block_image_bytes[block.block] += block.size;
                }
            }
            static constexpr size_t kOrdered = 1024;
            std::vector<std::pair<double, ImageId>> candidates;
            lru_cache.ForEachItemBelow(below, [&](ImageId id) {
                const auto block = slot_images[id].MemoryBlock();
                double fill = 0.5;
                if (block.block != 0 && block.block_size != 0) {
                    const auto it = block_image_bytes.find(block.block);
                    fill = it != block_image_bytes.end()
                               ? static_cast<double>(it->second) / static_cast<double>(block.block_size)
                               : 0.0;
                }
                candidates.emplace_back(fill, id);
                return candidates.size() >= kOrdered;
            });
            std::ranges::stable_sort(candidates, {}, &std::pair<double, ImageId>::first);
            for (const auto& entry : candidates) {
                if (visit(entry.second)) return;
            }
            if (candidates.size() < kOrdered) return;
        }
        lru_cache.ForEachItemBelow(below, visit);
    };
    // Images the GPU wrote are copied back to guest memory before they go, behind a wait for the
    // GPU. Eden evicts them from its "expected" mark on; here they stay, as they do below that
    // mark, until memory is really short (performance.h, KeepDirtyTextures).
    const auto DirtyEvictions = [&] {
        return high_priority_mode &&
               (total_used_memory >= critical_memory || !::Eden::Performance::KeepDirtyTextures());
    };
    // One batch of copies behind a single wait for the GPU: this many images and bytes at most.
    // A pass frees up to 40 images at a time (num_iterations), and at a resolution above 1x most
    // of what the GPU wrote is a few MiB each.
    static constexpr size_t kBatchImages = 64;
    static constexpr size_t kBatchBytes = 64_MiB;
    // While the pool is short, a GPU-written image larger than a batch stays and clean images go
    // instead: its write-back takes a staging copy, a swizzle buffer and guest memory for the
    // pages it fills, so evicting it costs more memory than it frees at the worst moment (Ocarina
    // of Time's 288 MiB 12288x12288 texture: the pool ran out with every write-back). Dropping the
    // write-back instead would leave stale content in an image the game keeps reusing.
    const auto KeepLargeDirty = [&](const Image& image) {
        if (image.unswizzled_size_bytes <= kBatchBytes || !::Eden::Performance::GraphicsMemoryIsShort())
            return false;
        static u32 kept{};
        if (kept++ < 8) {
            LOG_INFO(HW_GPU, "Texture cache: pool short, keeping GPU-written {} MiB at {:#x}",
                     image.unswizzled_size_bytes >> 20, image.gpu_addr);
        }
        return true;
    };
    using DownloadMap = decltype(runtime.DownloadStagingBuffer(size_t{}));
    boost::container::small_vector<std::pair<ImageId, DownloadMap>, 16> gc_downloads;
    const auto ReleaseDownloads = [&] {
        if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {
            for (auto& [id, map] : gc_downloads)
                runtime.FreeDeferredStagingBuffer(map);
            gc_downloads.clear();
        }
    };
    SCOPE_EXIT { ReleaseDownloads(); };
    const auto PrefetchDownloads = [&] {
        if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {
            ReleaseDownloads();
            size_t remaining = num_iterations;
            size_t bytes = 0;
            u64 usage = total_used_memory;
            // Only look ahead through independent dirty evictions. Leave all
            // deletion, alias updates and pressure-policy decisions to Cleanup.
            // ForEachItemBelow stops when the callback returns true (common/lru_cache.h).
            ForEachCandidate(frame_tick - ticks_to_destroy, [&](ImageId id) {
                if (remaining == 0) return true;
                --remaining;
                auto& image = slot_images[id];
                if (True(image.flags & ImageFlagBits::IsDecoding)) return false;
                const bool dirty = IsDownloadable(image) && False(image.flags & ImageFlagBits::BadOverlap);
                // Costly images as Cleanup has them: past Eden's own critical mark, or old ones while
                // the pool is short (gc_old_costly).
                if ((!(aggressive_mode && total_used_memory >= critical_memory) && !::Eden::Performance::gc_old_costly &&
                     True(image.flags & ImageFlagBits::CostlyLoad)) ||
                    (!DirtyEvictions() && dirty)) return false;
                // A clean image has nothing to copy back, and an image that does not fit the
                // remaining byte budget keeps the drain it already had in Cleanup. Neither may
                // end the lookahead: stopping here left every later dirty image to Cleanup's
                // per-image drain, one GPU pipeline wait each, where this loop would have spent
                // a single wait on all of them. Skip them and keep looking.
                if (!dirty || image.unswizzled_size_bytes > kBatchBytes - bytes) return false;
                // Aliased, overlapping and MSAA images stay with Cleanup: their copies are not
                // independent of the state that eviction updates. A scaled image is copied here
                // as Cleanup copies it, by the same call (it is scaled down for the copy): at a
                // resolution above 1x nearly every image the GPU wrote is scaled, and leaving
                // them out left a pass of sixty images with sixty waits, half a second in which
                // no frame was shown.
                if (!image.aliased_images.empty() || !image.overlapping_images.empty() ||
                    image.info.num_samples != 1 || gc_downloads.size() == kBatchImages)
                    return true;
                auto map = runtime.DownloadStagingBuffer(image.unswizzled_size_bytes, true);
                // Pin until CPU publication, even if later recording submits work.
                gc_downloads.emplace_back(id, map);
                const auto copies = FullDownloadCopies(image.info);
                image.DownloadMemory(map, copies);
                bytes += image.unswizzled_size_bytes;
                usage -= ReclaimedBytes(image);
                // The original policy changes at these thresholds. Stop looking
                // ahead here so we never copy a texture that it would retain.
                return usage < (aggressive_mode || ::Eden::Performance::KeepDirtyTextures() ? critical_memory
                                                                                           : expected_memory);
            });
            if (!gc_downloads.empty()) {
                // One drain for the whole batch; the image count is the reported payload.
                const ::Eden::Performance::Timer drain(::Eden::Performance::gc_batched_drain,
                                                       gc_downloads.size());
                runtime.Finish();
            }
        }
    };
    const auto UsePrefetched = [&](ImageId id, Image& image) {
        if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {
            const auto it = std::ranges::find_if(gc_downloads, [id](const auto& entry) {
                return entry.first == id;
            });
            if (it != gc_downloads.end()) {
                const auto copies = FullDownloadCopies(image.info);
                SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies,
                             it->second.mapped_span, swizzle_data_buffer);
                runtime.FreeDeferredStagingBuffer(it->second);
                gc_downloads.erase(it);
                return true;
            }
        }
        return false;
    };
    // The copy of an image Cleanup is about to free: from the batch, and when the batch is used
    // up and this image came after it (the batch was full, or the lookahead had stopped), from a
    // new batch that starts here. Without that every image after the first batch waited alone.
    const auto TakePrefetched = [&](ImageId id, Image& image) {
        if (UsePrefetched(id, image)) return true;
        if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {
            if (!gc_downloads.empty()) return false;
            PrefetchDownloads();
            return UsePrefetched(id, image);
        }
        return false;
    };
