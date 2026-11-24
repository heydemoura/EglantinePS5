// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/async_draw_policy.h"

TEST_CASE("Native pending draws are never discarded", "[video_core][async_draw]") {
    constexpr std::array<u32, 6> counts{0, 1, 6, 7, 4096, std::numeric_limits<u32>::max()};
    for (const bool depth : {false, true}) {
        for (const u32 indices : counts) {
            for (const u32 vertices : counts) {
                REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(true, depth, indices, vertices));
            }
        }
    }
}

/// EDEN_NX2_ASYNC_DISCARD=1: native draws use the legacy heuristic; non-native draws are unchanged.
TEST_CASE("Native pending draws follow the async heuristic when opted in",
          "[video_core][async_draw]") {
    REQUIRE(Vulkan::CanDiscardPendingDraw(true, true, 0, 0, true));
    REQUIRE(Vulkan::CanDiscardPendingDraw(true, false, 7, 7, true));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(true, false, 6, 100, true));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(true, false, 0, 0, true));
    // The opt-in never changes non-native draws.
    REQUIRE(Vulkan::CanDiscardPendingDraw(false, true, 0, 0, true) ==
            Vulkan::CanDiscardPendingDraw(false, true, 0, 0));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(false, false, 0, 0, true));
}

TEST_CASE("Legacy pending draws retain the async heuristic", "[video_core][async_draw]") {
    REQUIRE(Vulkan::CanDiscardPendingDraw(false, true, 0, 0));
    REQUIRE(Vulkan::CanDiscardPendingDraw(false, false, 7, 7));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(false, false, 6, 100));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(false, false, 100, 6));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(false, false, 0, 0));
}

TEST_CASE("A native draw needs history on every render target it writes, through the RT map",
          "[video_core][async_draw]") {
    const auto identity = [](u32 i) { return i; };
    const auto to_rt1 = [](u32) { return 1u; };
    const auto writes_all = [](u32) { return true; };
    const auto rt0_only = [](u32 rt) { return rt == 0; };
    const auto none = [](u32) { return false; };
    // Output 0 maps to RT1: RT0's history must not let a first write into RT1 be skipped.
    REQUIRE_FALSE(Vulkan::ColorWritesHaveHistory(1, to_rt1, writes_all, rt0_only));
    REQUIRE(Vulkan::ColorWritesHaveHistory(1, to_rt1, writes_all, writes_all));
    REQUIRE(Vulkan::ColorWritesHaveHistory(1, identity, writes_all, rt0_only));
    // Two outputs, one into a target first written this frame.
    REQUIRE_FALSE(Vulkan::ColorWritesHaveHistory(2, identity, writes_all, rt0_only));
    // A masked output needs no history; a draw that writes no color is not skippable.
    REQUIRE(Vulkan::ColorWritesHaveHistory(2, identity, rt0_only, rt0_only));
    REQUIRE_FALSE(Vulkan::ColorWritesHaveHistory(2, identity, none, writes_all));
    REQUIRE_FALSE(Vulkan::ColorWritesHaveHistory(0, identity, writes_all, writes_all));
}
