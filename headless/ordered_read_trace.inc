// SPDX-License-Identifier: GPL-3.0-or-later
// Included once by performance.cpp. The ordered-read capture and the readback experiments it
// measured relied on core changes that the Lichii core replaced (patch 0004, dropped); only the
// Vulkan worker hand-off switch remains (vulkan_scheduler.cpp asks eden_vulkan_handoff_enabled).
#include "ordered_read_trace.h"
#include <atomic>
#include <cstdio>

namespace Eden::OrderedReadTrace {
static std::atomic<bool> vulkan_handoff_enabled{};

void Frame(std::uint64_t frame, std::uint64_t) {
    if (frame % 10 != 0) return;
    const char* handoff_path = "/data/homebrew/PPSA98515/mkw-handoff-mode.txt";
    if (FILE* mode = std::fopen(handoff_path, "r")) {
        unsigned value{};
        if (std::fscanf(mode, "%u", &value) == 1 && value <= 1) {
            vulkan_handoff_enabled.store(value != 0, std::memory_order_release);
            std::printf("MKW_HANDOFF_MODE frame=%llu mode=%u\n",
                static_cast<unsigned long long>(frame), value);
        }
        std::fclose(mode);
        std::remove(handoff_path);
    }
}
}

extern "C" bool eden_vulkan_handoff_enabled() {
    return Eden::OrderedReadTrace::vulkan_handoff_enabled.load(std::memory_order_acquire);
}
