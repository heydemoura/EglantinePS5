// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/sparse_large_vector.h"
#include <time.h>

namespace Common {
void* ReserveMemoryRange(std::size_t size) noexcept;
void* ReserveAddressRange(std::size_t size) noexcept;
bool CommitMemoryRange(void* address, std::size_t size) noexcept;
bool CommitMemoryRangeFrom(void* address, std::size_t size, bool high) noexcept;
int DecommitMemoryRange(void* address, std::size_t size) noexcept;
int PageOutRange(void* address, std::size_t size) noexcept;
int PageInRange(void* address, std::size_t size) noexcept;
void ReleaseMemoryRange(void* address, std::size_t size) noexcept;
} // namespace Common

// Out of memory for the heap: the guest memory service packs cold guest memory to give some back
// (src/guest_memory_service.inc; never waits, the heap may hold its growth lock), and the heap
// asks once more. Before, a refused growth made an allocation fail (a worker thread then read
// address 0x199).
extern "C" [[gnu::weak]] std::size_t eden_memory_recover_try(std::size_t bytes) noexcept;
template <typename Try>
static auto WithRecovery(std::size_t size, Try&& attempt) {
    auto result = attempt();
    // More than once: what came back can go to another taker (a guest write, the driver) before the
    // heap asks again, and when nothing came back the service may find memory nothing touches a
    // moment later (it looks every second). Half a second when the service answers at once, and
    // 2 s at most: a request may wait half a second for whoever is packing, or write hundreds of
    // MiB to the swap file, and the heap may hold its lock here.
    timespec start{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int tries = 0; tries < 20 && !result && &eden_memory_recover_try != nullptr; ++tries) {
        if (eden_memory_recover_try(size + (std::size_t{8} << 20)) == 0) {
            const timespec pause{0, 25'000'000};
            nanosleep(&pause, nullptr);
        }
        result = attempt();
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000 >= 2000) break;
    }
    return result;
}

// Reuse the qualified, owned direct-memory backend for the C mspace heap. One block of the pool, and
// no memory asked back when the pool has no free block that large: what the service gives back are
// 2 MiB blocks from all over the pool, which do not make one, and the heap then takes the block in
// pieces anyway (eden_heap_split_pages, which does ask). For an 88 MiB block that Mario Kart World
// allocates when it streams a part of a track in, 93 MiB of guest memory was packed or written to
// the swap file, up to twenty times over, while the thread that asked waited.
extern "C" void* eden_heap_pages(std::size_t size) {
    return Common::AllocateMemoryPages(size);
}
extern "C" void eden_heap_pages_free(void* base, std::size_t size) {
    Common::FreeMemoryPages(base, size);
}
// The heap that grows (headless/heap_arenas.inc): address space first, memory piece by piece.
extern "C" void* eden_heap_reserve(std::size_t size) {
    return Common::ReserveMemoryRange(size);
}
extern "C" int eden_heap_commit(void* address, std::size_t size) {
    return WithRecovery(size, [&] { return Common::CommitMemoryRange(address, size); }) ? 0 : -1;
}
// A piece that holds nothing goes back to the system; its address space stays the heap's.
// 1: given back, 0: nothing done, -1: given back in part (not to be used again).
extern "C" int eden_heap_decommit(void* address, std::size_t size) {
    return Common::DecommitMemoryRange(address, size);
}
// A released arena's memory (headless/heap_arenas.inc): part of a committed block, in 16 KiB pages.
extern "C" int eden_heap_page_out(void* address, std::size_t size) {
    return Common::PageOutRange(address, size);
}
extern "C" int eden_heap_page_in(void* address, std::size_t size) {
    return WithRecovery(size, [&] { return Common::PageInRange(address, size) == 1; }) ? 1 : 0;
}
// A large block whose memory the pool has in no single free block: address space of its own,
// with memory behind it in blocks of whatever size the pool has (zeroed), given back whole.
extern "C" void* eden_heap_split_pages(std::size_t size) {
    constexpr std::size_t large_page = std::size_t{2} << 20;
    if (size == 0 || size > ~std::size_t{0} - large_page) return nullptr;
    const std::size_t span = (size + large_page - 1) / large_page * large_page;
    void* address = Common::ReserveAddressRange(span);
    if (address == nullptr) return nullptr;
    if (!WithRecovery(span, [&] { return Common::CommitMemoryRangeFrom(address, span, false); })) {
        Common::ReleaseMemoryRange(address, span);
        return nullptr;
    }
    return address;
}
extern "C" void eden_heap_split_pages_free(void* address, std::size_t size) {
    constexpr std::size_t large_page = std::size_t{2} << 20;
    Common::ReleaseMemoryRange(address, (size + large_page - 1) / large_page * large_page);
}
