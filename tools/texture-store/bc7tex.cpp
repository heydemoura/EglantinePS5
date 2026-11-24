// SPDX-License-Identifier: GPL-3.0-or-later
// From PS5_Lichii (mihawk, codeberg.org/mihawk/PS5_Lichii, commit 5a424f7), ps5/tools/texture-store/bc7tex.cpp.
//
// Whole-image BC7 (bc7e.ispc) and BC5 (rgbcx) encoders for texture_store.py, called through ctypes.
// Blocks past the right and bottom edge repeat the edge pixels, so padding never costs quality.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include "bc7e_ispc.h"
#include "common/cityhash.h"
#include "rgbcx.h"

static void block_row(const uint8_t* rgba, int w, int h, int by, uint32_t* px) {
    const uint32_t* src = (const uint32_t*)rgba;
    int wb = (w + 3) / 4;
    for (int bx = 0; bx < wb; bx++)
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                px[bx * 16 + y * 4 + x] = src[std::min(by * 4 + y, h - 1) * w + std::min(bx * 4 + x, w - 1)];
}

extern "C" {

void tex_init() {
    ispc::bc7e_compress_block_init();
    rgbcx::init();
}

// The emulator's own content hash (Eglantine/src/common/cityhash.cpp), so store keys match by construction.
uint64_t tex_cityhash64(const char* data, size_t size) {
    return Common::CityHash64(data, size);
}

// level: 0 basic, 1 slow, 2 veryslow, 3 slowest. Error is plain mean square (not perceptual), alpha weighted by
// alpha_weight against 1 for each colour channel.
// bc7e does not count alpha in an opaque block and its mode 6 can then decode alpha as 254. With exact_alpha such
// blocks are encoded again without mode 6, so an opaque texel stays exactly 255.
void tex_bc7(const uint8_t* rgba, int w, int h, int level, int exact_alpha, int alpha_weight, uint8_t* out) {
    ispc::bc7e_compress_block_params p;
    if (level <= 0) ispc::bc7e_compress_block_params_init_basic(&p, false);
    else if (level == 1) ispc::bc7e_compress_block_params_init_slow(&p, false);
    else if (level == 2) ispc::bc7e_compress_block_params_init_veryslow(&p, false);
    else ispc::bc7e_compress_block_params_init_slowest(&p, false);
    p.m_weights[3] = alpha_weight;
    ispc::bc7e_compress_block_params no6 = p;
    no6.m_opaque_settings.m_use_mode[6] = false;
    int wb = (w + 3) / 4, hb = (h + 3) / 4;
    std::vector<uint32_t> px(wb * 16), again;
    std::vector<int> where;
    std::vector<uint64_t> redo;
    for (int by = 0; by < hb; by++) {
        uint8_t* row = out + (size_t)by * wb * 16;
        block_row(rgba, w, h, by, px.data());
        ispc::bc7e_compress_blocks(wb, (uint64_t*)row, px.data(), &p);
        if (!exact_alpha) continue;
        again.clear();
        where.clear();
        for (int bx = 0; bx < wb; bx++) {
            const uint8_t* b = row + bx * 16;
            bool mode6 = (b[0] & 0x7F) == 0x40, pbits = (b[7] & 0x80) && (b[8] & 1);   // bits 63 and 64
            if (!mode6 || pbits) continue;
            uint32_t all = 0xFF000000;
            for (int i = 0; i < 16; i++) all &= px[bx * 16 + i];
            if (all != 0xFF000000) continue;   // a block with real alpha: the encoder counted it
            where.push_back(bx);
            again.insert(again.end(), &px[bx * 16], &px[bx * 16] + 16);
        }
        redo.resize(where.size() * 2);
        if (!where.empty()) ispc::bc7e_compress_blocks(where.size(), redo.data(), again.data(), &no6);
        for (size_t i = 0; i < where.size(); i++) memcpy(row + where[i] * 16, &redo[i * 2], 16);
    }
}

// c0, c1: which RGBA channels become the block's red and green. radius: endpoint search radius.
void tex_bc5(const uint8_t* rgba, int w, int h, int c0, int c1, int radius, uint8_t* out) {
    int wb = (w + 3) / 4, hb = (h + 3) / 4;
    std::vector<uint32_t> px(wb * 16);
    for (int by = 0; by < hb; by++) {
        block_row(rgba, w, h, by, px.data());
        for (int bx = 0; bx < wb; bx++)
            rgbcx::encode_bc5_hq(out + ((size_t)by * wb + bx) * 16, (const uint8_t*)&px[bx * 16], c0, c1, 4, radius);
    }
}

}
