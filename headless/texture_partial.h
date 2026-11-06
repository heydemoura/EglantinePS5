// SPDX-License-Identifier: GPL-3.0-or-later
// Port of ProsperoEden's partial texture refresh (headless/texture_partial.h there).
//
// A game streaming small tiles into a large texture (a 5120x5120 BC4 atlas, 17.8 MB) made Eden
// upload the whole image again after each burst of writes, 4-8 ms of GPU-thread work each, up to nine
// times in a frame: 40-75 ms frames. Here a guest write to a large block-linear image whose contents
// the GPU already holds stops tracking only the 4 KiB pages it wrote, and the next use uploads the
// block rows those pages hold.
//
// The Eglantine core already has that mechanism (TextureCache::MarkPartialWrite and
// RefreshPartialContents, for 2D images of 16 MiB or more, block-granular, every level and layer).
// ProsperoEden's own copy of it takes images from 1 MiB on; on top of the core's this port only
// sets that bound (tools/prepare-vulkan-port.py) and adds the check and the counters below.
// dev-settings tex_partial=off restores the core's own 16 MiB bound, tex_partial_kb=N sets the
// smallest image (default 1024), tex_partial_check=on also unswizzles the whole image and compares
// (EDEN_TEX_PARTIAL_MISMATCH; counted in EDEN_DEV_GUEST tex_partial*). Texture cache lock held.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "performance.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/types.h"
#include "video_core/texture_cache/util.h"

namespace Eden::TexturePartial {
// Whether the rows a partial refresh unswizzled (`rows`, laid out as `copies`) match the same texels
// of a whole-image unswizzle of the image as guest memory holds it now. A guest write to a written
// page between the two reads shows as a mismatch (rare; the counters are for development runs).
template <class MemoryManager, class Copies>
bool CheckRows(MemoryManager& gpu_memory, const VideoCommon::ImageBase& image, const Copies& copies,
               std::span<const u8> rows) {
    const VideoCommon::ImageInfo& info = image.info;
    const u32 bpp = VideoCore::Surface::BytesPerBlock(info.format);
    const u32 tile_w = VideoCore::Surface::DefaultBlockWidth(info.format);
    const u32 tile_h = VideoCore::Surface::DefaultBlockHeight(info.format);
    std::vector<u8> guest(image.guest_size_bytes);
    gpu_memory.ReadBlockUnsafe(image.gpu_addr, guest.data(), guest.size());
    std::vector<u8> whole(VideoCommon::CalculateUnswizzledSizeBytes(info));
    const auto whole_copies = VideoCommon::UnswizzleImage(gpu_memory, image.gpu_addr, info, guest, whole);
    for (const VideoCommon::BufferImageCopy& copy : copies) {
        const s32 level = copy.image_subresource.base_level;
        if (level < 0 || static_cast<size_t>(level) >= whole_copies.size()) return false;
        const auto& level_copy = whole_copies[level];
        // The whole unswizzle packs each level's layers one after the other, rows tightly.
        const u32 level_tiles_w = Common::DivCeil(level_copy.image_extent.width, tile_w);
        const u32 level_tiles_h = Common::DivCeil(level_copy.image_extent.height, tile_h);
        const size_t layer_bytes = size_t{level_tiles_w} * level_tiles_h * bpp;
        const u32 run_tiles = copy.buffer_row_length / tile_w;
        const u32 run_rows = copy.buffer_image_height / tile_h;
        const u32 x_tile = static_cast<u32>(copy.image_offset.x) / tile_w;
        const u32 y_tile = static_cast<u32>(copy.image_offset.y) / tile_h;
        if (x_tile >= level_tiles_w) continue;
        // Tiles past the image's width (block padding) are in the rows only.
        const u32 compare_tiles = (std::min)(run_tiles, level_tiles_w - x_tile);
        for (u32 row = 0; row < run_rows && y_tile + row < level_tiles_h; ++row) {
            const size_t whole_at = level_copy.buffer_offset +
                                    size_t(copy.image_subresource.base_layer) * layer_bytes +
                                    (size_t{y_tile + row} * level_tiles_w + x_tile) * bpp;
            const size_t rows_at = copy.buffer_offset + size_t{row} * run_tiles * bpp;
            const size_t bytes = size_t{compare_tiles} * bpp;
            if (whole_at + bytes > whole.size() || rows_at + bytes > rows.size() ||
                std::memcmp(whole.data() + whole_at, rows.data() + rows_at, bytes) != 0) {
                return false;
            }
        }
    }
    return true;
}

template <class MemoryManager, class Copies>
void Check(MemoryManager& gpu_memory, const VideoCommon::ImageBase& image, const Copies& copies,
           std::span<const u8> rows) {
    const bool same = CheckRows(gpu_memory, image, copies, rows);
    Eden::Performance::tex_partial_checks.fetch_add(1, std::memory_order_relaxed);
    if (!same && Eden::Performance::tex_partial_mismatches.fetch_add(1, std::memory_order_relaxed) < 50) {
        std::printf("EDEN_TEX_PARTIAL_MISMATCH addr=%llx format=%u size=%ux%u levels=%d layers=%d block=%u "
                    "copies=%zu",
                    static_cast<unsigned long long>(image.gpu_addr), unsigned(image.info.format),
                    image.info.size.width, image.info.size.height, image.info.resources.levels,
                    image.info.resources.layers, image.info.block.height, size_t(copies.size()));
        for (size_t i = 0; i < copies.size() && i < 8; ++i)
            std::printf(" L%d/%d:x%d,y%d+%ux%u", copies[i].image_subresource.base_level,
                        copies[i].image_subresource.base_layer, copies[i].image_offset.x,
                        copies[i].image_offset.y, copies[i].image_extent.width, copies[i].image_extent.height);
        std::printf("\n");
    }
}
} // namespace Eden::TexturePartial
