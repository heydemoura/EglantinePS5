// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Tegra::Texture::ASTC {

void Decompress(std::span<const uint8_t> data, uint32_t width, uint32_t height, uint32_t depth,
                uint32_t block_width, uint32_t block_height, std::span<uint8_t> output);

/// True if no 16-byte block in `data` decodes to alpha below 1.
bool IsOpaque(std::span<const uint8_t> data);

enum class AlphaKind {
    Opaque, ///< alpha 1 everywhere
    Binary, ///< alpha only 0 or 1 (within the tolerance): cut-outs, BC1 punch-through keeps it
    Other,  ///< some alpha in between
};

struct AlphaStats {
    uint64_t texels;      ///< texels scanned (whole blocks)
    uint64_t transparent; ///< of them, alpha below 0.5
    uint64_t between;     ///< alpha neither 0 nor 1
    uint32_t max_between; ///< largest distance of an alpha byte from 0 or 255 (255: not decodable)
};

/// Alpha statistics of ASTC `data`; stops once an alpha byte is more than `stop_above` from 0 and
/// 255 (127 or more scans everything).
AlphaStats ScanAlpha(std::span<const uint8_t> data, uint32_t block_width, uint32_t block_height,
                     uint32_t stop_above);

/// What the alpha of `data` (ASTC blocks of block_width x block_height texels) holds. Blocks
/// whose modes carry no alpha are not decoded; the scan stops at the first in-between alpha.
/// `tolerance` (0-127): alpha bytes within it of 0 or 255 still count as binary.
AlphaKind ClassifyAlpha(std::span<const uint8_t> data, uint32_t block_width, uint32_t block_height,
                        uint32_t tolerance);

} // namespace Tegra::Texture::ASTC
