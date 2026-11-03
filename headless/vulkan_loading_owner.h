// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>

namespace Eden {

// The loading UI and guest frames share the renderer, scheduler and composition
// layers. Once GPU splitting starts, both must record on the backend owner.
// Recheck after earlier work: its first guest frame may have ended loading.
template <typename Split, typename Work>
bool RunVulkanLoadingOnOwner(Split& split, std::atomic<bool>& loading, Work work) {
    if (!loading.load(std::memory_order_acquire)) {
        return false;
    }
    if (split.Active() && !Split::IsBackThread()) {
        split.Defer([&loading, work]() {
            if (loading.load(std::memory_order_acquire)) {
                work();
            }
        });
        split.Drain();
    } else if (loading.load(std::memory_order_acquire)) {
        work();
    }
    return loading.load(std::memory_order_acquire);
}

} // namespace Eden
