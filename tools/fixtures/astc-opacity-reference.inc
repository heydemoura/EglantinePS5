// Frozen pre-optimization opacity parser from the maintained Eglantine core.
// SPDX-License-Identifier: GPL-3.0-or-later
bool Reference(std::span<const uint8_t> data) {
    // Endpoint modes with an alpha channel: LDR luminance+alpha (4, 5), RGB scale + two alpha
    // (10), LDR RGBA (12, 13) and HDR RGBA (14, 15).
    constexpr u32 AlphaModes = (1U << 4) | (1U << 5) | (1U << 10) | (1U << 12) | (1U << 13) |
                               (1U << 14) | (1U << 15);
    for (size_t offset = 0; offset + 16 <= data.size(); offset += 16) {
        const std::span<const u8> block = data.subspan(offset, 16);
        InputBitStream strm(block);
        const TexelWeightParams params = DecodeBlockInfo(strm);
        if (params.m_bError) {
            continue; // decodes to the opaque error colour
        }
        if (params.m_bVoidExtentLDR || params.m_bVoidExtentHDR) {
            // One RGBA16 colour in bits 64-127: UNORM16, or FP16 for HDR.
            const u32 alpha = block[14] | (u32{block[15]} << 8);
            if (alpha != (params.m_bVoidExtentHDR ? 0x3C00U : 0xFFFFU)) {
                return false;
            }
            continue;
        }
        const u32 partitions = strm.ReadBits<2>() + 1;
        if (partitions == 4 && params.m_bDualPlane) {
            continue;
        }
        if (partitions == 1) {
            if ((AlphaModes >> strm.ReadBits<4>()) & 1) {
                return false;
            }
            continue;
        }
        strm.ReadBits<10>(); // partition index
        const u32 base_cem = strm.ReadBits<6>();
        const u32 base_mode = base_cem & 3;
        if (base_mode == 0) {
            if ((AlphaModes >> (base_cem >> 2)) & 1) {
                return false;
            }
            continue;
        }
        // The rest of the per-partition modes sit just below the weights (DecompressBlock).
        const u32 weight_bits = params.GetPackedBitSize();
        if (weight_bits < 24 || weight_bits > 96) {
            continue;
        }
        const u32 extra_bits = partitions == 2 ? 2 : partitions == 3 ? 5 : 8;
        const u32 extra_offset = 128 - weight_bits - extra_bits;
        u32 extra = 0;
        for (u32 bit = 0; bit < extra_bits; ++bit) {
            const u32 position = extra_offset + bit;
            extra |= ((block[position / 8] >> (position % 8)) & 1U) << bit;
        }
        u32 cem = ((extra << 6) | base_cem) >> 2;
        const u32 classes = cem;
        cem >>= partitions;
        for (u32 i = 0; i < partitions; ++i) {
            const u32 mode = ((base_mode - (((classes >> i) & 1) != 0 ? 0 : 1)) << 2) |
                             ((cem >> (2 * i)) & 3);
            if ((AlphaModes >> mode) & 1) {
                return false;
            }
        }
    }
    return true;
}
