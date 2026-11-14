// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "video_core/host1x/ffmpeg.h"
#include "video_core/host1x/nx2_video_decoder.h"

namespace {
using namespace Tegra::Host1x;

Nx2Picture Picture() {
    auto frame = std::make_shared<FFmpeg::Frame>();
    auto* av = frame->GetFrame();
    av->format = AV_PIX_FMT_NV12;
    av->width = 8;
    av->height = 4;
    REQUIRE(av_frame_get_buffer(av, 32) == 0);
    for (int y = 0; y < 4; ++y) {
        std::fill_n(av->data[0] + y * av->linesize[0], 8, 100);
    }
    for (int y = 0; y < 2; ++y) {
        std::fill_n(av->data[1] + y * av->linesize[1], 8, 128);
    }
    return {.width = 8, .height = 4, .frame = std::move(frame)};
}
}

TEST_CASE("Packed video output rejects a luma-only pitch", "[nx2_video][pitch]") {
    const auto picture = Picture();
    Nx2VideoDecoder decoder{"video/unknown"};
    for (auto format : {Nx2PictureFormat::Rgba8, Nx2PictureFormat::Bgra8}) {
        for (u64 pitch : {8ULL, 31ULL}) {
            std::vector<u8> output(256, 0xcd);
            decoder.Render(picture, format, 8, 4, pitch, 0, output);
            REQUIRE(std::all_of(output.begin(), output.end(), [](u8 v) { return v == 0xcd; }));
        }
        std::vector<u8> output(256, 0xcd);
        decoder.Render(picture, format, 8, 4, 32, 0, output);
        REQUIRE(output[0] != 0xcd);
        REQUIRE(output[3] == 255);
        REQUIRE(std::all_of(output.begin() + 128, output.end(), [](u8 v) { return v == 0xcd; }));
    }
}

TEST_CASE("NV12 video retains its single-byte luma pitch", "[nx2_video][pitch]") {
    const auto picture = Picture();
    Nx2VideoDecoder decoder{"video/unknown"};
    std::vector<u8> output(64, 0xcd);
    decoder.Render(picture, Nx2PictureFormat::Nv12, 8, 4, 8, 32, output);
    REQUIRE(std::all_of(output.begin(), output.begin() + 32, [](u8 v) { return v == 100; }));
    REQUIRE(std::all_of(output.begin() + 32, output.begin() + 48, [](u8 v) { return v == 128; }));
    REQUIRE(std::all_of(output.begin() + 48, output.end(), [](u8 v) { return v == 0xcd; }));
}
