    if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {
        // Keep independent copies in one submission, then publish guest writes
        // in modification order. A staging allocation stays busy until its tick.
        using Map = decltype(runtime.DownloadStagingBuffer(size_t{}));
        std::vector<std::pair<ImageId, Map>> pending;
        pending.reserve(16);
        constexpr size_t batch_limit = 32_MiB;
        size_t batch_bytes = 0;
        const auto finish_batch = [&] {
            if (pending.empty()) return;
            runtime.Finish();
            for (auto& [image_id, map] : pending) {
                Image& image = slot_images[image_id];
                const auto copies = FullDownloadCopies(image.info);
                SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, map.mapped_span,
                             swizzle_data_buffer);
                image.flags &= ~ImageFlagBits::PartialDownload;
                std::erase(partial_download_images, image_id);
            }
            pending.clear();
            batch_bytes = 0;
        };
        // Research opt-in: ordered pre-write completion and alias transitions keep
        // partially visible GPU contents from being used as a whole CPU upload.
        static const bool partial_readbacks = [] {
            const char* value = std::getenv("EDEN_NX2_PARTIAL_TEXTURE_READBACK");
            return value && value[0] == '1' && value[1] == '\0';
        }();
        for (const ImageId image_id : images) {
            Image& image = slot_images[image_id];
            const size_t bytes = image.unswizzled_size_bytes;
            // Command-buffer reads can overlap a much larger GPU-written image. Publish only
            // the requested bytes and leave the remaining image dirty. Dense mappings only:
            // sparse/remapped image offsets need an explicit per-segment translation instead.
            if (partial_readbacks && gpu_memory->UsesNativeTiling() && bytes > batch_limit && size != 0 &&
                True(image.flags & ImageFlagBits::Tracked) &&
                cpu_addr <= ~DAddr{0} - size &&
                image.cpu_addr <= ~DAddr{0} - image.guest_size_bytes &&
                False(image.flags & (ImageFlagBits::Converted | ImageFlagBits::AcceleratedUpload |
                    ImageFlagBits::Unbacked | ImageFlagBits::Sparse | ImageFlagBits::SparseTiles |
                    ImageFlagBits::SparseLayers | ImageFlagBits::Remapped | ImageFlagBits::TrimmedLevels)) &&
                gpu_memory->IsContinuousRange(image.gpu_addr, image.guest_size_bytes) &&
                gpu_memory->GpuToCpuAddress(image.gpu_addr).value_or(~DAddr{0}) == image.cpu_addr) {
                const DAddr first = std::max(cpu_addr, image.cpu_addr);
                const DAddr last = std::min(cpu_addr + size, image.cpu_addr + image.guest_size_bytes);
                if (last > first) {
                    const auto bands = PartialDownloadBands(image.info, first - image.cpu_addr,
                                                             size_t(last - first), 4_MiB);
                    // A full readback of a newer overlapping image could otherwise clear
                    // its dirty bit while this older partial image remains dirty. A later
                    // read must never republish the older pixels over that newer result.
                    bool exclusive = true;
                    if (bands) {
                        ForEachImageInRegion(image.cpu_addr, image.guest_size_bytes,
                            [&](ImageId other_id, ImageBase&) {
                                if (other_id != image_id) exclusive = false;
                            });
                    }
                    if (bands && exclusive) {
                        finish_batch();
                        // DownloadMemory's selection cleared this bit in advance; a partial
                        // synchronization does not certify the image's untouched contents.
                        if (False(image.flags & ImageFlagBits::PartialDownload))
                            partial_download_images.push_back(image_id);
                        image.flags |= ImageFlagBits::GpuModified | ImageFlagBits::PartialDownload;
                        partial_downloads_seen.store(true, std::memory_order_release);
                        std::vector<u8> guest_band;
                        size_t transferred{};
                        for (const auto& band : *bands) {
                            auto map = runtime.DownloadStagingBuffer(band.copy.buffer_size, true);
                            const std::span<const BufferImageCopy> copies{&band.copy, 1};
                            image.DownloadMemory(map, copies);
                            runtime.Finish();
                            guest_band.resize(band.guest_size);
                            gpu_memory->ReadBlockUnsafe(image.gpu_addr + band.guest_offset,
                                                        guest_band.data(), band.guest_size);
                            SwizzleRowBand(image.info, band.copy, map.mapped_span, guest_band, true);
                            gpu_memory->WriteBlockUnsafe(image.gpu_addr + band.write_offset,
                                guest_band.data() + band.write_offset - band.guest_offset, band.write_size);
                            runtime.FreeDeferredStagingBuffer(map);
                            transferred += band.copy.buffer_size;
                        }
                        if (static unsigned traces{}; traces++ < 64)
                            std::printf("MKW_PARTIAL_READBACK image=%zu request=%zu transferred=%zu bands=%zu dirty=1\n",
                                        bytes, size, transferred, bands->size());
                        continue;
                    }
                }
            }
            // Bounded eligibility audit: record why a real oversized readback stayed whole.
            if (partial_readbacks && bytes > batch_limit) {
                static u32 rejected{};
                if (rejected++ < 64) {
                    const bool address_ok = size && cpu_addr <= ~DAddr{0} - size &&
                        image.cpu_addr <= ~DAddr{0} - image.guest_size_bytes;
                    const bool continuous = gpu_memory->IsContinuousRange(image.gpu_addr, image.guest_size_bytes);
                    const auto mapped = gpu_memory->GpuToCpuAddress(image.gpu_addr);
                    const DAddr first = address_ok ? std::max(cpu_addr, image.cpu_addr) : 0;
                    const DAddr last = address_ok ? std::min(cpu_addr + size, image.cpu_addr + image.guest_size_bytes) : 0;
                    const auto bands = last > first ? PartialDownloadBands(image.info,
                        first - image.cpu_addr, size_t(last - first), 4_MiB) : std::nullopt;
                    u32 overlaps{};
                    ForEachImageInRegion(image.cpu_addr, image.guest_size_bytes,
                        [&](ImageId other_id, ImageBase&) { overlaps += other_id != image_id; });
                    std::printf("MKW_PARTIAL_REJECT request=%llx+%zu image=%llx+%zu linear=%zu flags=%x "
                                "continuous=%u mapped=%llx bands=%d overlaps=%u type=%u format=%u "
                                "dimensions=%ux%ux%u levels=%u layers=%u block=%ux%ux%u spacing=%u\n",
                        static_cast<unsigned long long>(cpu_addr), size,
                        static_cast<unsigned long long>(image.cpu_addr), size_t(image.guest_size_bytes), bytes,
                        static_cast<u32>(image.flags), unsigned(continuous),
                        static_cast<unsigned long long>(mapped.value_or(~DAddr{0})),
                        bands ? int(bands->size()) : -1, overlaps, unsigned(image.info.type),
                        unsigned(image.info.format), image.info.size.width, image.info.size.height,
                        image.info.size.depth, image.info.resources.levels, image.info.resources.layers,
                        image.info.block.width, image.info.block.height, image.info.block.depth,
                        image.info.tile_width_spacing);
                }
            }
            if (bytes > batch_limit &&
                False(image.flags & (ImageFlagBits::Converted | ImageFlagBits::AcceleratedUpload |
                                     ImageFlagBits::Unbacked))) {
                const auto bands = DownloadRowBands(image.info, 4_MiB);
                if (!bands.empty()) {
                    finish_batch();
                    std::vector<u8> guest_band;
                    for (const auto& copy : bands) {
                        auto map = runtime.DownloadStagingBuffer(copy.buffer_size, true);
                        const std::span<const BufferImageCopy> copies{&copy, 1};
                        image.DownloadMemory(map, copies);
                        runtime.Finish();
                        const auto [offset, count] = RowBandGuestRange(image.info, copy);
                        guest_band.resize(count);
                        // Preserve padding and untouched texels just as the whole-image
                        // UnsafeReadWrite mapping does. Pin the map through CPU publication.
                        gpu_memory->ReadBlockUnsafe(image.gpu_addr + offset, guest_band.data(), count);
                        SwizzleRowBand(image.info, copy, map.mapped_span, guest_band,
                                       gpu_memory->UsesNativeTiling());
                        gpu_memory->WriteBlockUnsafe(image.gpu_addr + offset, guest_band.data(), count);
                        runtime.FreeDeferredStagingBuffer(map);
                    }
                    image.flags &= ~ImageFlagBits::PartialDownload;
                    std::erase(partial_download_images, image_id);
                    if (static unsigned reports{}; reports++ < 16)
                        std::printf("MKW_BANDED_READBACK bytes=%zu bands=%zu limit=4194304\n", bytes,
                                    bands.size());
                    continue;
                }
            }
            if (pending.size() == 16 || bytes > batch_limit - batch_bytes)
                finish_batch();
            auto map = runtime.DownloadStagingBuffer(bytes);
            const auto copies = FullDownloadCopies(image.info);
            image.DownloadMemory(map, copies);
            pending.emplace_back(image_id, std::move(map));
            // Oversized images retain the original one-image allocation/wait.
            if (bytes >= batch_limit) {
                finish_batch();
            } else {
                batch_bytes += bytes;
            }
        }
        finish_batch();
        return;
    }
