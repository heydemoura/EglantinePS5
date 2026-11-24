// SPDX-License-Identifier: GPL-3.0-or-later
// ---- The guest's memory as a service: shared until written (included by memory_pages.cpp) ----
//
// After Gregory Price's "A Compressed RAM Service" (Linux Plumbers 2026, mm/cram.c): memory that
// is read where it lies and promoted when it is written. There the device compresses; here the
// emulator is the device, and the two forms a processor can read without a fault are a slot that
// shares the block of zeroes and a slot that shares a block with another slot of the same bytes.
//
//   Zero    the slot shows the shared block of zeroes, read-only
//   Own     the slot has a block of its own, read-write
//   Shared  the slot shows a block it shares with other slots of the same bytes, read-only
//   Fine    the slot is 128 pages of 16 KiB, read-only: its pages of zeroes show the zero block, the
//           others a compact block that holds only them (a written slot with many pages of zeroes);
//           a write makes it whole again, and it waits longer each time before it may be Fine again
//
//   Packed  the slot has no block: its data is LZ4-compressed in the packed store, or in the swap store
//           on the console's storage ("Swap" below); any access unpacks it
//   Idle    the slot keeps its block but allows no access: the first access of any kind wakes it. How
//           the scanner learns which slots nothing reads or writes; only those are packed or swapped
//   Clean   the slot has its block back from the swap store, read-only, and the store still holds the
//           same bytes: giving the block up again costs no write to the storage. A write makes it Own
//
// A write to a Zero or Shared slot faults; HandleGuestWriteFault gives the slot a block of its
// own (zeroed, or a copy) and the write repeats. So memory is no longer taken when the guest's
// kernel hands pages out, only when they are written; a slot the guest's kernel clears or frees
// whole goes back to the zero block; and a scanner gives back slots that hold only zeroes and
// lets slots of the same bytes share one block. A slot is locked (bit 7 of its state) while it
// changes; a writer that faults on a locked slot waits for it.
//
// 64-bit games only: a 32-bit game's fastmem window aliases the backing, which takes it whole.

namespace {
enum : std::uint8_t { SlotZero = 0, SlotOwn = 1, SlotShared = 2, SlotFine = 3, SlotPacked = 4, SlotIdle = 5, SlotClean = 6, SlotLock = 0x80, SlotMask = 0x7f };
constexpr std::size_t FinePage = 16384, FinePages = SparseSlot / FinePage;
// A written slot becomes Fine with at least this many pages of zeroes (512 KiB given back).
constexpr std::size_t FineMinZeroPages = 32;
// A slot made whole again by a write waits 1, 2, 4 ... 1024 scan passes before it may be Fine again.
constexpr unsigned FineBackoffMax = 10;

// Duplicate sharing is off unless asked for (dev-settings cram_dedup=on): on the console it found a
// dozen 2 MiB pairs that were written apart within a minute, and with it on a large game held 20 FPS
// instead of 30 (the GPU worker waited for every fence, once a frame) where it held 30 without.
std::atomic<bool> service_requested{true}, scan_requested{true}, dedup_requested{false}, fine_requested{true};
std::atomic<SparseRange*> service_range{nullptr}; // the guest range the service runs for
std::atomic<SparseRange*> probe_range{nullptr};   // the self-test's range
std::atomic<int> service_state{0};                // 0 not tested, 1 works here, 2 does not
const char* service_refused = "";

struct Counters {
    std::atomic<std::uint64_t> faults{0}, promotions{0}, copies{0}, waits{0};
    std::atomic<std::uint64_t> returns_clear{0}, returns_free{0}, returns_scan{0}, dedup{0};
    std::atomic<std::uint64_t> reserve_used{0}, host_reserve_used{0}, failures{0}, passes{0}, scan_ns{0};
    std::atomic<std::uint64_t> fined{0}, unfined{0}, fine_failures{0};
};
Counters counters;

// ---- The 64-bit fastmem window's aliases (src/host_memory.cpp, Window64; backported from ProsperoEden) ----
// A window region (2 MiB) shows a slot's block from a JIT fault until the slot's block changes. Every
// change of a slot's block or protection (a promotion, a return, a scan's fence) first drops its
// aliases, under the slot's lock: a stale alias would reach memory the slot gave up. An entry is the
// window address, plus AliasReadOnly when it shows the block read-only, or AliasSmall when it shows
// 16 KiB pieces of it rather than the whole block.
constexpr unsigned AliasesPerSlot = 4;
constexpr std::uintptr_t AliasReadOnly = 1, AliasSmall = 2, AliasFlags = AliasReadOnly | AliasSmall;
std::atomic<std::uint64_t> aliases_made{0}, aliases_dropped{0}, aliases_refused{0};
// The window asked for a slot or a page at least once (EDEN_FASTMEM64=1): a slot in 64 KiB units
// then comes back whole at a touch, as the window can only show whole blocks ("Units" below).
std::atomic<bool> window_seen{false};
// The window region at `at` shows nothing again: a reservation straight over the mapping.
void VoidWindowRegion([[maybe_unused]] std::uintptr_t at) noexcept {
#ifdef PS5_NATIVE
    void* address = reinterpret_cast<void*>(at);
    if (sceKernelReserveVirtualRange(&address, SparseSlot, MAP_FIXED, SparseSlot) == 0 && address == reinterpret_cast<void*>(at))
        return;
    address = reinterpret_cast<void*>(at);
    if (munmap(address, SparseSlot) != 0 || sceKernelReserveVirtualRange(&address, SparseSlot, MAP_FIXED, SparseSlot) != 0 ||
        address != reinterpret_cast<void*>(at))
        std::abort(); // a window region left mapped would expose memory the slot gives up
#else
    if (mmap(reinterpret_cast<void*>(at), SparseSlot, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) ==
        MAP_FAILED)
        std::abort();
#endif
}
// Slot locked.
void DropSlotAliases(SparseRange& range, std::size_t slot) noexcept {
    if (!range.guest || !range.aliases) return;
    for (unsigned i = 0; i < AliasesPerSlot; ++i)
        if (auto& alias = range.aliases[slot * AliasesPerSlot + i]) {
            VoidWindowRegion(alias & ~AliasFlags);
            alias = 0;
            aliases_dropped.fetch_add(1, std::memory_order_relaxed);
        }
}
void ResetCounters() noexcept {
    for (auto* counter : {&counters.faults, &counters.promotions, &counters.copies, &counters.waits,
                          &counters.returns_clear, &counters.returns_free, &counters.returns_scan, &counters.dedup,
                          &counters.reserve_used, &counters.host_reserve_used, &counters.failures, &counters.passes, &counters.scan_ns,
                          &counters.fined, &counters.unfined, &counters.fine_failures})
        counter->store(0, std::memory_order_relaxed);
}

// Freed pages cleared (GuestMemoryFreedPart, below).
std::atomic<std::uint64_t> freed_part_calls{0}, freed_part_bytes{0};
// Idle blocks RecoverMemory gave back (GiveBackIdleBlocks).
std::atomic<std::uint64_t> idle_given_back{0}, idle_give_back_calls{0};
// What the keeper had given back before an allocation failed (KeepFree).
std::atomic<std::uint64_t> keeper_calls{0}, keeper_bytes{0};

bool LockSlot(SparseRange& range, std::size_t slot, bool count_wait = false) noexcept {
    auto& state = range.state[slot];
    bool waited = false;
    for (;;) {
        std::uint8_t value = state.load(std::memory_order_relaxed);
        if (!(value & SlotLock) &&
            state.compare_exchange_weak(value, value | SlotLock, std::memory_order_acquire, std::memory_order_relaxed))
            break;
        waited = true;
        sched_yield();
    }
    if (waited && count_wait) counters.waits.fetch_add(1, std::memory_order_relaxed);
    return waited;
}
// The lock, if the slot is not locked now. For whoever holds pack_mutex: a fault handler keeps its
// slot locked while it waits for a block, and asks for pack_mutex meanwhile to have memory given
// back (WaitForBlock). Waiting for that slot here would hold both up until the handler gave up,
// 50 s later, with its access refused. A locked slot is in use: it is left for another time.
bool TryLockSlot(SparseRange& range, std::size_t slot) noexcept {
    auto& state = range.state[slot];
    std::uint8_t value = state.load(std::memory_order_relaxed);
    return !(value & SlotLock) &&
           state.compare_exchange_strong(value, value | SlotLock, std::memory_order_acquire, std::memory_order_relaxed);
}
std::size_t FineKept(SparseRange& range, std::size_t slot) noexcept {
    return range.fine[slot] & 0xff;
}
std::uint8_t SlotState(SparseRange& range, std::size_t slot) noexcept {
    return range.state[slot].load(std::memory_order_acquire) & SlotMask;
}
// Sets the state and lets the slot go.
void UnlockSlot(SparseRange& range, std::size_t slot, std::uint8_t to) noexcept {
    // Packed: no block. Idle and Clean: a block that may not be written yet (CommitSparsePage must
    // come through ServiceCommit).
    range.owned[slot].store(to != SlotZero && to != SlotPacked && to != SlotIdle && to != SlotClean, std::memory_order_release);
    range.state[slot].store(to, std::memory_order_release);
}
std::uintptr_t SlotAddress(SparseRange& range, std::size_t slot) noexcept {
    return range.begin.load(std::memory_order_relaxed) + slot * SparseSlot;
}

// ---- Blocks: the pool, a reserve for when the pool says no, and shared blocks' references ----
//
// The reserve is the guest's headroom in the pool, held as blocks of its own: 1 GiB that a game's
// writes take when the pool has no block left. It used to be 32 MiB here, plus 1 GiB that the
// graphics caches only counted as taken (headless/performance.cpp): that 1 GiB stayed in the pool,
// where Eden's heap, the driver and the caches themselves could take it, and where 2 MiB blocks
// could no longer be cut from what was left. Tears of the Kingdom's writes found no block four
// times with the pool's largest free block at 1 or 2 MiB. Held here, nothing else can take it or
// cut it up; the caches see the pool as it is, less what is not held yet, and evict as soon as a
// burst of writes has used half of it (performance.h ViewOfPool). It is topped up every 100 ms
// (ReserveKeeper) and given back with the guest's memory when the session ends.
constexpr std::size_t ReserveBlocks = 256;      // 512 MiB (diagnostic62 shared-pool test)
constexpr std::size_t ReserveAtStart = 16;      // taken as the service starts; the keeper takes the rest
std::int64_t reserve[ReserveBlocks];
bool PackEnabled() noexcept; // packing (below)
// The reserve kept, in blocks: EDEN_CRAM_RESERVE_MIB (up to 512), else 128 MiB while packing runs
// (packing gives blocks back when the pool is short; MKW used none of the 512 MiB), else 512 MiB.
std::size_t ReserveTarget() noexcept {
    static const std::size_t blocks = [] {
        const char* value = std::getenv("EDEN_CRAM_RESERVE_MIB");
        const std::size_t mib = value != nullptr ? std::strtoull(value, nullptr, 10) : PackEnabled() ? 128 : 512;
        return std::max<std::size_t>(1, std::min(ReserveBlocks, mib / (SparseSlot >> 20)));
    }();
    return blocks;
}
std::atomic<std::size_t> reserve_count{0};
std::atomic_flag reserve_lock = ATOMIC_FLAG_INIT;
constexpr std::size_t SharedBlocks = 4096;
std::int64_t shared_block[SharedBlocks];
int shared_refs[SharedBlocks];
std::atomic_flag shared_lock = ATOMIC_FLAG_INIT;

void Spin(std::atomic_flag& flag) noexcept {
    while (flag.test_and_set(std::memory_order_acquire)) sched_yield();
}

// ---- Packed slots: an Own slot unwritten for a while, LZ4-compressed while the pool is short ----
// The slot maps the zero block with no access and its block goes back; the next access of any
// kind faults, and the fault handler gives it a block and unpacks it there (UnpackSlot). Packed
// data lives in the heap. A buffer the fault handler is done with is freed by the scanner (no
// free in a signal handler): it goes on a list through its first word.
struct PackedSlot {
    void* data = nullptr;       // in the packed store (memory); null when the data is in the swap store
    std::uint32_t size = 0;     // bytes in the packed store
    std::uint32_t swap = 0;     // not 0: the swap store has the slot's bytes (its units' slots are in swap_units)
    std::uint64_t hash = 0;     // swap store only: FullHash of the slot's 2 MiB, checked when read back
    bool units = false;         // the slot is in 64 KiB units ("Units"): data is its packed units' piece, and
                                // swap_units says where each unit is
};
std::atomic<PackedSlot*> packed_slots{nullptr}; // per slot of the guest's range, made by the scanner
// Diagnostic (EDEN_CRAM_PACK_KEEP=1): a packed slot keeps its block, and the unpack compares what it
// unpacked with it. A difference means the slot's data changed while it was packed (written through a
// mapping other than the slot's own). Saves no memory: for finding what packing breaks.
bool PackKeep() noexcept {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_CRAM_PACK_KEEP");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}
std::int64_t* pack_kept = nullptr; // per slot: the block a packed slot kept, -1 none
// Pages of each slot the GPU's caches hold (eden_guest_gpu_cached, from the core's device memory
// manager): such a slot is not packed. With packing, Mario Kart World's emulated GPU read impossible
// draw arguments (1.8-2.9 billion indices) and the GPU hung, only in sessions where the GPU thread
// itself touched packed slots; the slots' data came back intact, so what the GPU reads through its
// caches and their ordering must not meet a packed slot. That left MKW ~86 MiB to pack (it maps
// nearly all its memory for the GPU), and it still ran out; the impossible draws come without
// packing too. So such slots are packed by default; EDEN_CRAM_PACK_GPU=0 keeps them out.
std::atomic<std::atomic<std::int32_t>*> gpu_cached_table{nullptr};
std::size_t gpu_cached_slots = 0;
std::atomic<std::uint64_t> pack_gpu_skips{0}, pack_gpu_marked{0};
// Slots a GPU thread (the command reader "GPU", or "GPUBack") ever had to unpack: never packed again.
// The caches' count misses what the command reader reads (command lists, draw arguments), and one
// such unpack was enough for impossible draws.
std::atomic<std::uint8_t>* gpu_touched = nullptr;
// Diagnostic: which threads touched packed slots (unpacks by thread name, 16 names at most).
struct Unpacker {
    char name[24];
    std::atomic<std::uint32_t> count;
};
Unpacker unpackers[16];
std::atomic_flag unpackers_lock = ATOMIC_FLAG_INIT;
void CountUnpacker(std::size_t slot) noexcept;
std::atomic<std::uint64_t> pack_keep_compared{0}, pack_keep_differed{0};
std::atomic<std::uint64_t> packed_count{0}, packed_bytes{0}, packs{0}, unpacks{0}, pack_skips{0};

// The packed store: packed data lives in 2 MiB blocks of direct memory of its own, filled one after
// another, outside Eden's heap. Packing then takes no heap memory, so it can run where the heap
// itself found no memory (eden_memory_recover_try), and leaves no slack in the heap. A block whose
// data all went back is given back by the scanner (StoreReclaim). When the pool has no block for
// it, the slot being packed gives its own. Taken and filled under pack_mutex; StoreRelease (the
// fault handler) only counts.
constexpr std::size_t StoreBlock = SparseSlot, StoreBlocks = 2048; // up to 4 GiB of packed data
std::uintptr_t store_base = 0;
std::int64_t store_physical[StoreBlocks];
std::uint32_t store_used[StoreBlocks];
std::atomic<std::uint32_t> store_live[StoreBlocks];
std::size_t store_current = StoreBlocks;
std::atomic<std::size_t> store_held{0};

#ifdef PS5_NATIVE
bool StoreStart() noexcept {
    if (store_base != 0) return true;
    void* address = reinterpret_cast<void*>(cpu_mapping_hint);
    if (sceKernelReserveVirtualRange(&address, StoreBlock * StoreBlocks, 0, StoreBlock) != 0) return false;
    store_base = reinterpret_cast<std::uintptr_t>(address);
    for (std::size_t i = 0; i < StoreBlocks; ++i) store_physical[i] = -1;
    return true;
}
// Room for `size` bytes (pack_mutex held). *donate: a block the caller gives up if the pool has
// none (-1 afterwards when it was taken).
void* StoreAlloc(std::size_t size, std::int64_t* donate) noexcept {
    if (store_current < StoreBlocks && store_used[store_current] + size <= StoreBlock) {
        void* at = reinterpret_cast<void*>(store_base + store_current * StoreBlock + store_used[store_current]);
        store_used[store_current] += static_cast<std::uint32_t>(size);
        store_live[store_current].fetch_add(static_cast<std::uint32_t>(size), std::memory_order_relaxed);
        return at;
    }
    std::size_t index = 0;
    while (index < StoreBlocks && store_physical[index] >= 0) ++index;
    if (index == StoreBlocks) return nullptr;
    std::int64_t physical = -1;
    if (eden_direct_low(StoreBlock, StoreBlock, 12, &physical) != 0) {
        if (donate == nullptr || *donate < 0) return nullptr;
        physical = std::exchange(*donate, -1);
    }
    if (!MapSlot(store_base + index * StoreBlock, PROT_READ | PROT_WRITE, physical)) {
        if (donate != nullptr && *donate < 0) *donate = physical;
        else (void)sceKernelReleaseDirectMemory(physical, StoreBlock);
        return nullptr;
    }
    store_physical[index] = physical;
    store_used[index] = static_cast<std::uint32_t>(size);
    store_live[index].store(static_cast<std::uint32_t>(size), std::memory_order_relaxed);
    store_current = index;
    store_held.fetch_add(StoreBlock, std::memory_order_relaxed);
    return reinterpret_cast<void*>(store_base + index * StoreBlock);
}
// Blocks whose data all went back (pack_mutex held).
void StoreReclaim() noexcept {
    // Nothing before the store has started: its tables are not set up (zero is not "no block"),
    // and mapping at store_base 0 would land on the app's own code (the self-test's range goes
    // back through here at start).
    if (store_base == 0) return;
    for (std::size_t i = 0; i < StoreBlocks; ++i) {
        if (store_physical[i] < 0 || i == store_current || store_live[i].load(std::memory_order_acquire) != 0) continue;
        if (!MapSlot(store_base + i * StoreBlock, PROT_NONE, zero_block)) continue;
        if (sceKernelReleaseDirectMemory(store_physical[i], StoreBlock) != 0) std::abort();
        store_physical[i] = -1;
        store_held.fetch_sub(StoreBlock, std::memory_order_relaxed);
    }
}
#endif
std::uint64_t StoreMoved() noexcept; // what StoreCompact moved so far (below)
void StoreRelease(void* data, std::uint32_t size) noexcept {
    const std::size_t index = (reinterpret_cast<std::uintptr_t>(data) - store_base) / StoreBlock;
    store_live[index].fetch_sub(size, std::memory_order_release);
}
// ---- Swap: slots that do not pack, in a store on the console's storage ----
// The packed store gives back what LZ4 saves, and nothing for a slot that does not compress: the
// textures, models and sound a game keeps in its own memory once the GPU has its copy. At a Mario
// Kart World race load thousands of such slots were looked at and left (skips=3456) while the pool
// ran out. With a store on the console's storage behind the packed store such a slot's block goes
// back too.
//
// The store is PS5_Lichii's (mihawk, codeberg.org/mihawk/PS5_Lichii: Ps5SwapOpen and Ps5SwapOut in
// src/common/host_memory.cpp there), and it is used as it is there, because the storage's flash
// wears with every byte written and this store writes few:
//   - <cache>/swap/store.bin is slots of 64 KiB, index.bin the hashes of their contents in slot
//     order. A slot is found by its contents: 64 KiB that are to go are hashed, looked up, and
//     compared byte for byte with the slot the hash names. What the store holds already costs no
//     write.
//   - Both files stay between sessions. A game's own data, loaded as it was the last time, is found
//     there again in every later session.
//   - A slot is never written twice, and the store only grows, up to EDEN_SWAP_MIB (8 GiB). What a
//     session writes is what the store grew by.
//   - Writes are kept under 8 GiB an hour, with 1 GiB to start (EDEN_SWAP_RATE_MIB_HOUR,
//     EDEN_SWAP_BURST_MIB), and the store leaves EDEN_SWAP_KEEP_FREE_MIB of the storage free.
// Three things are not as there. A store with less than a quarter of its size left starts over at
// the next launch, once a day at most (SwapStart), where Lichii's takes nothing new once it is
// full. Slots go to it when an allocation has failed, not before (PackPass has why). And the budget
// is twice Lichii's (SwapStart has why).
// A slot of the guest's memory is 2 MiB here, where a unit is 64 KiB there. It goes as 32 units,
// each found or appended on its own, and a unit of zeroes takes no slot of the store.
// A request for memory after a failed allocation sends the slots nothing touches and that will not
// pack there (RecoverMemory); the scanner's pass can do so ahead of any, as Lichii's does (PackPass,
// EDEN_SWAP_PASS).
// Safe: a lookup compares bytes, so neither two contents with one hash nor an index that a power
// cut left ahead of its store can pass for a slot's data. A slot of the guest's memory is hashed
// whole when it goes and checked when it comes back (Lichii trusts the store there); data that does
// not come back as it was ends the session rather than reaching the game. A failed write leaves
// the slot as it was and ends swapping. Since no slot of the store is ever rewritten, a fault
// handler reads it without any lock against the one writer (the holder of pack_mutex).
// EDEN_SWAP=0 turns it off; it is off without storage access (no EDEN_SWAP_DIR).
constexpr std::size_t SwapUnit = 64 * 1024;                              // a slot of the store
constexpr std::size_t SwapUnits = SparseSlot / SwapUnit;                 // of them in a slot of the guest's memory
constexpr std::size_t SwapMaxSlots = (std::size_t{8} << 30) / SwapUnit;  // the store: 8 GiB at most
constexpr std::size_t SwapTableSize = SwapMaxSlots * 2;                  // (a power of two)
constexpr std::uint32_t SwapZeroUnit = 0xFFFFFFFFu;                      // a unit of zeroes: no slot of the store
int swap_store_fd = -1, swap_index_fd = -1;
std::atomic<int> swap_state{0}; // 0 not started, 1 on, 2 off
// The store's slots (pack_mutex held: one writer).
std::uint32_t swap_count = 0, swap_carried = 0; // held now; left by earlier sessions
std::uint32_t swap_cap = 0, swap_limit = 0;     // most it may hold (EDEN_SWAP_MIB); and with the storage's free space as it is
std::uint32_t* swap_table = nullptr;                            // slot + 1 by content hash, open addressing
std::uint64_t* swap_hashes = nullptr;                           // each slot's content hash
// Slots read back to be compared (SwapHeld): one, or the run after it once a second in a row is asked for.
std::uint8_t* swap_check = nullptr;
// A slot's new units, side by side for their one write (SwapOutSlot).
std::uint8_t* swap_gather = nullptr;
std::uint32_t swap_check_first = 0, swap_check_slots = 0, swap_check_last = 0xFFFFFFFFu;
// The budget (pack_mutex held).
std::int64_t swap_tokens = 0, swap_burst = 0, swap_rate = 0; // bytes; bytes a second
std::uint64_t swap_token_ns = 0, swap_keep_free = 0;
char swap_folder[448];
// (bytes appended this session; units appended, found in the store and of zeroes; 2 MiB slots read back)
std::atomic<std::uint64_t> swap_written{0}, swap_appended{0}, swap_found{0}, swap_zero_units{0}, swap_write_ns{0};
std::atomic<std::uint64_t> swap_appends{0}; // the writes those bytes went in (one for a slot's new units)
std::atomic<std::uint64_t> swap_reads{0}, swap_read_ns{0}, swap_read_max_ns{0};
// (slots of the guest's memory whose bytes the store has now, given up again without a write, dropped by a write)
std::atomic<std::uint64_t> swap_slots{0}, swap_reused{0}, swap_dirtied{0};
std::atomic<std::uint64_t> swap_denied_rate{0}, swap_full{0}, swap_missed{0};

std::uint64_t FullHash(std::uintptr_t at, std::size_t bytes) noexcept;
std::size_t EnvOr(const char* name, std::size_t fallback) noexcept;
bool PackKeep() noexcept;

std::uint64_t SwapNow() noexcept {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(now.tv_nsec);
}
bool SwapOn() noexcept { return swap_state.load(std::memory_order_acquire) == 1; }
// Why it is off, for the report (SwapOff may run in a fault handler: no stdio there).
std::atomic<const char*> swap_off_why{"not needed yet"};
std::atomic<int> swap_off_errno{0};
void SwapOff(const char* why) noexcept {
    const int error = errno;
    if (swap_state.exchange(2, std::memory_order_acq_rel) == 2) return;
    swap_off_errno.store(error, std::memory_order_relaxed);
    swap_off_why.store(why, std::memory_order_release);
    Note("EDEN_SWAP off\n");
}
// Whole, or not at all.
bool SwapWriteAt(int fd, const void* from, std::size_t size, std::uint64_t offset) noexcept {
    const auto* bytes = static_cast<const char*>(from);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t wrote = pwrite(fd, bytes + done, size - done, static_cast<off_t>(offset + done));
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) break;
        done += static_cast<std::size_t>(wrote);
    }
    return done == size;
}
// In a fault handler too.
bool SwapReadAt(int fd, void* to, std::size_t size, std::uint64_t offset) noexcept {
    auto* bytes = static_cast<char*>(to);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t got = pread(fd, bytes + done, size - done, static_cast<off_t>(offset + done));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        done += static_cast<std::size_t>(got);
    }
    return done == size;
}

// A unit's content hash: Lichii's Ps5Hash, so that a store reads the same there and here.
std::uint64_t SwapUnitHash(const std::uint8_t* bytes) noexcept {
    std::uint64_t hash = 0x9E3779B97F4A7C15ull;
    for (std::size_t at = 0; at < SwapUnit; at += 8) {
        std::uint64_t word;
        std::memcpy(&word, bytes + at, 8);
        hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
        hash ^= hash >> 29;
    }
    return hash;
}
bool SwapUnitZero(const std::uint8_t* bytes) noexcept {
    for (std::size_t at = 0; at < SwapUnit; at += 64) {
        std::uint64_t words[8];
        std::memcpy(words, bytes + at, 64);
        if (words[0] | words[1] | words[2] | words[3] | words[4] | words[5] | words[6] | words[7]) return false;
    }
    return true;
}
// (The table is half empty at most, so a place is a few steps away. An index gone wrong, with one
// hash thousands of times over, would make it thousands for each: such a slot is left out, and is
// then one the store holds and no longer finds.)
void SwapTableInsert(std::uint64_t hash, std::uint32_t slot) noexcept {
    std::size_t at = hash & (SwapTableSize - 1);
    for (unsigned steps = 0; swap_table[at] != 0; at = (at + 1) & (SwapTableSize - 1))
        if (++steps == 4096) return;
    swap_table[at] = slot + 1;
}

// How many slots the store may hold now: its limit, and what the storage has free beyond
// EDEN_SWAP_KEEP_FREE_MIB (looked at when it opens and once a pass).
void SwapRoom() noexcept {
    swap_cap = static_cast<std::uint32_t>(
        std::min<std::size_t>(EnvOr("EDEN_SWAP_MIB", 8192), SwapMaxSlots * SwapUnit >> 20) * ((std::size_t{1} << 20) / SwapUnit));
    // What the storage has free: not known (the folder is gone, say) is none, and the store does
    // not grow. (A file system inside its reserve has less than none, which FreeBSD hands through
    // as a huge count.)
    struct statvfs space {};
    std::uint64_t room = 0;
    if (statvfs(swap_folder, &space) == 0 && space.f_frsize != 0 && space.f_bavail <= space.f_bfree) {
        const std::uint64_t free_bytes = static_cast<std::uint64_t>(space.f_bavail) * static_cast<std::uint64_t>(space.f_frsize);
        room = free_bytes > swap_keep_free ? (free_bytes - swap_keep_free) / SwapUnit : 0;
    }
    swap_limit = static_cast<std::uint32_t>(std::min<std::uint64_t>(swap_cap, swap_count + std::min<std::uint64_t>(room, swap_cap)));
}

// The store's files, and the slots earlier sessions left in them: opened when the pool is first
// short (the scanner), or when memory is first asked back by the driver (pack_mutex held; never in
// a fault handler or under the heap's lock: it allocates).
bool SwapStart() noexcept {
    if (const int state = swap_state.load(std::memory_order_acquire); state != 0) return state == 1;
    errno = 0;
    const char* const off = std::getenv("EDEN_SWAP");
    const char* const folder = std::getenv("EDEN_SWAP_DIR");
    if (off != nullptr && off[0] == '0') return SwapOff("switched off"), false;
    if (folder == nullptr || folder[0] == 0 || std::strlen(folder) >= sizeof(swap_folder)) return SwapOff("no folder (no storage access)"), false;
    if (PackKeep()) return SwapOff("EDEN_CRAM_PACK_KEEP"), false;
    // (6 MiB of the heap. A request for memory may be the first to come here, with the pool empty
    // and the heap unable to grow: not for good then, the next pass or request tries again.)
    if (swap_table == nullptr) swap_table = new (std::nothrow) std::uint32_t[SwapTableSize];
    if (swap_hashes == nullptr) swap_hashes = new (std::nothrow) std::uint64_t[SwapMaxSlots];
    if (swap_check == nullptr) swap_check = new (std::nothrow) std::uint8_t[SparseSlot];
    if (swap_gather == nullptr) swap_gather = new (std::nothrow) std::uint8_t[SparseSlot];
    if (swap_table == nullptr || swap_hashes == nullptr || swap_check == nullptr || swap_gather == nullptr) return false;
    std::memset(swap_table, 0, SwapTableSize * sizeof(std::uint32_t));
    std::snprintf(swap_folder, sizeof(swap_folder), "%s", folder);
    (void)mkdir(swap_folder, 0777);
    char path[sizeof(swap_folder) + 16];
    std::snprintf(path, sizeof(path), "%s/store.bin", swap_folder);
    const int store = open(path, O_RDWR | O_CREAT, 0666);
    std::snprintf(path, sizeof(path), "%s/index.bin", swap_folder);
    const int index = open(path, O_RDWR | O_CREAT, 0666);
    struct stat status {};
    if (store < 0 || index < 0 || fstat(store, &status) != 0) {
        const int error = errno;
        if (store >= 0) close(store);
        if (index >= 0) close(index);
        // (Off for good at the fifth time only: the first may be a moment's lack of something.)
        static unsigned refused = 0;
        if (++refused < 5) return false;
        errno = error;
        return SwapOff("its files could not be opened"), false;
    }
    // A slot counts when both its hash and all of its bytes are there (a session may have ended
    // between the two).
    const std::size_t whole = std::min<std::size_t>(static_cast<std::size_t>(status.st_size) / SwapUnit, SwapMaxSlots);
    std::size_t hashes = 0;
    while (hashes < whole * sizeof(std::uint64_t)) {
        const ssize_t got = pread(index, reinterpret_cast<char*>(swap_hashes) + hashes, whole * sizeof(std::uint64_t) - hashes,
                                  static_cast<off_t>(hashes));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        hashes += static_cast<std::size_t>(got);
    }
    swap_count = swap_carried = static_cast<std::uint32_t>(hashes / sizeof(std::uint64_t));
    swap_check_slots = 0;
    swap_check_last = 0xFFFFFFFFu;
    swap_store_fd = store;
    swap_index_fd = index;
    swap_keep_free = std::uint64_t{EnvOr("EDEN_SWAP_KEEP_FREE_MIB", 1024)} << 20;
    SwapRoom();
    // A store with less than a quarter of its size left (its size: not what the storage's free
    // space keeps it to) starts over, here, where no slot of the guest's memory is in it yet, and
    // not more often than every EDEN_SWAP_START_OVER_HOURS (24; `started` in its folder holds when
    // it last did). Lichii's stays as it is once it is full and takes only what it holds already
    // (EDEN_SWAP_START_OVER_HOURS=0 here). A game whose memory differs from one session to the next
    // (Mario Kart World: a third of what a session sends is found again in the next, a 30-minute
    // session adds about 2 GiB and an hour 7) fills 8 GiB in little more than an hour of play, and
    // would have no store left to send anything new to. A quarter, because a session that starts
    // with less room than that is one that ends at its next large need. So the storage takes at
    // most the store's size for each time it starts, and it starts once a day at most.
    //
    // That bound is only as good as `started`, so the time goes to the storage first, and for
    // good, and a store whose time cannot be written does not start over. A time from after now
    // (the clock was ahead then, or is behind now: a console that has lost its clock) is left as
    // it is, and the store waits for it to come: taken for a clock that was wrong then, a console
    // whose clock is wrong at every other launch would start over at every other launch. (Deleting
    // the folder is the way out of a time far ahead.)
    bool started_over = false;
    {
        std::snprintf(path, sizeof(path), "%s/started", swap_folder);
        const std::uint64_t now = static_cast<std::uint64_t>(time(nullptr));
        const std::uint64_t every = std::uint64_t{EnvOr("EDEN_SWAP_START_OVER_HOURS", 24)} * 3600;
        std::uint64_t started = 0;
        const int mark = open(path, O_RDWR | O_CREAT, 0666);
        if (mark >= 0 && pread(mark, &started, sizeof(started), 0) != static_cast<ssize_t>(sizeof(started))) started = 0;
        const auto note = [&]() noexcept {
            return mark >= 0 && pwrite(mark, &now, sizeof(now), 0) == static_cast<ssize_t>(sizeof(now)) && fsync(mark) == 0;
        };
        const bool full = swap_count != 0 && swap_cap - std::min(swap_cap, swap_count) < std::max<std::uint32_t>(swap_cap / 4, 1);
        // (A store without a time, a new one or one from before there was one: its day starts now.)
        const bool due = every != 0 && started <= now && (started == 0 || now - started >= every);
        if (full && due) {
            // The index first: without it the store is empty, whatever store.bin still holds (its
            // slots are then written over as the store grows).
            if (note() && ftruncate(index, 0) == 0) {
                (void)ftruncate(store, 0);
                swap_count = swap_carried = 0;
                started_over = true;
                SwapRoom();
            }
        } else if (started == 0) {
            (void)note();
        }
        if (mark >= 0) close(mark);
    }
    for (std::uint32_t slot = 0; slot < swap_count; ++slot) SwapTableInsert(swap_hashes[slot], slot);
    // 1 GiB at once and 8 GiB an hour, where Lichii has 512 MiB and 4 GiB: with those a Mario Kart
    // World session written to on request only ran out of budget at its second large load (a track's
    // textures took the 512 MiB at 22 minutes, and the next load, five minutes later, wanted more
    // than had been refilled) and ended after 29 minutes.
    swap_burst = static_cast<std::int64_t>(EnvOr("EDEN_SWAP_BURST_MIB", 1024)) << 20;
    swap_rate = (static_cast<std::int64_t>(EnvOr("EDEN_SWAP_RATE_MIB_HOUR", 8192)) << 20) / 3600;
    swap_tokens = swap_burst;
    swap_token_ns = SwapNow();
    swap_state.store(1, std::memory_order_release);
    char line[400];
    const int n = std::snprintf(line, sizeof(line),
        "EDEN_SWAP on: %s holds %lluMiB from earlier sessions%s, may hold %lluMiB; burst=%lldMiB rate=%lldMiB/h\n", swap_folder,
        (unsigned long long)(std::uint64_t{swap_carried} * SwapUnit >> 20), started_over ? " (it was full and starts over)" : "",
        (unsigned long long)(std::uint64_t{swap_limit} * SwapUnit >> 20), (long long)(swap_burst >> 20),
        (long long)((swap_rate * 3600 + (1 << 19)) >> 20));
    if (n > 0) Report(line);
    return true;
}

// May `bytes` more be written now (pack_mutex held)? `urgent`: memory was refused, not just short;
// a quarter of the burst is kept for that.
bool SwapBudget(std::size_t bytes, bool urgent) noexcept {
    const std::uint64_t elapsed_ms = (SwapNow() - swap_token_ns) / 1000000ull;
    swap_tokens = std::min<std::int64_t>(swap_burst, swap_tokens + static_cast<std::int64_t>(elapsed_ms) * swap_rate / 1000);
    swap_token_ns += elapsed_ms * 1000000ull;
    if (swap_tokens < static_cast<std::int64_t>(bytes) + (urgent ? 0 : swap_burst / 4)) {
        swap_denied_rate.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

// A slot of the store as it is on the storage, to be compared: null when it cannot be read. One
// slot is read, or the run that follows once the slot after the last one is asked for (the units of
// a slot of the guest's memory that went together lie in a row: the store only appends).
const std::uint8_t* SwapHeld(std::uint32_t slot) noexcept {
    if (slot < swap_check_first || slot - swap_check_first >= swap_check_slots) {
        const std::uint32_t slots =
            slot == swap_check_last + 1 ? std::min<std::uint32_t>(static_cast<std::uint32_t>(SwapUnits), swap_count - slot) : 1;
        swap_check_slots = 0;
        if (!SwapReadAt(swap_store_fd, swap_check, std::size_t{slots} * SwapUnit, std::uint64_t{slot} * SwapUnit)) return nullptr;
        swap_check_first = slot;
        swap_check_slots = slots;
    }
    swap_check_last = slot;
    return swap_check + std::size_t{slot - swap_check_first} * SwapUnit;
}
// The slot of the store that holds these 64 KiB, + 1; 0 when none does (pack_mutex held).
std::uint32_t SwapFind(const std::uint8_t* bytes, std::uint64_t hash) noexcept {
    unsigned others = 0;
    for (std::size_t at = hash & (SwapTableSize - 1); swap_table[at] != 0; at = (at + 1) & (SwapTableSize - 1)) {
        const std::uint32_t slot = swap_table[at] - 1;
        if (swap_hashes[slot] != hash) continue;
        const std::uint8_t* const held = SwapHeld(slot);
        if (held != nullptr && std::memcmp(held, bytes, SwapUnit) == 0) return slot + 1;
        // (Each of those is a read of the storage. A hash that many slots have and none matches
        // says nothing: an index gone wrong, see SwapTableInsert.)
        if (++others == 8) break;
    }
    return 0;
}
// `count` units in a row, appended as as many slots: the first slot + 1, or 0 when the write failed
// (pack_mutex held; the budget and the store's room were asked before).
std::uint32_t SwapAppend(const std::uint8_t* bytes, const std::uint64_t* hashes, std::size_t count) noexcept {
    const std::uint32_t first = swap_count;
    const std::uint64_t start = SwapNow();
    // The bytes, then their hashes: a slot counts when both are there.
    const bool ok = SwapWriteAt(swap_store_fd, bytes, count * SwapUnit, std::uint64_t{first} * SwapUnit) &&
                    SwapWriteAt(swap_index_fd, hashes, count * sizeof(std::uint64_t), std::uint64_t{first} * sizeof(std::uint64_t));
    swap_write_ns.fetch_add(SwapNow() - start, std::memory_order_relaxed);
    if (!ok) return 0;
    for (std::size_t i = 0; i < count; ++i) {
        swap_hashes[first + i] = hashes[i];
        SwapTableInsert(hashes[i], first + static_cast<std::uint32_t>(i));
    }
    swap_count += static_cast<std::uint32_t>(count);
    swap_tokens -= static_cast<std::int64_t>(count * SwapUnit);
    swap_written.fetch_add(count * SwapUnit, std::memory_order_relaxed);
    swap_appended.fetch_add(count, std::memory_order_relaxed);
    swap_appends.fetch_add(1, std::memory_order_relaxed);
    return first + 1;
}

// (pack_mutex held) The budget or the store's room refused a slot's writes: the caller stops asking for more.
bool swap_refused = false;
// How a slot may go to the store: only if that writes nothing (all of it is there already); with
// writes while the budget keeps its part for failed allocations; with writes down to its last byte.
enum class SwapWrites { None, Budget, Urgent };
// Slots in 64 KiB units ("Units", further down; the host's check below uses these): what a unit's
// record in swap_units says besides a slot of the store + 1.
constexpr std::uint32_t UnitZero = SwapZeroUnit, UnitHere = 0x40000000u, UnitPacked = 0x80000000u, UnitIndex = 0x3FFFFFFFu;
constexpr std::size_t UnitPackMax = SwapUnit * 3 / 4; // (Lichii's: 48 of 64 KiB)
bool UnitZstdStart() noexcept;
std::size_t UnitsOut(const std::uint8_t* bytes, std::uint32_t mask, std::uint32_t* units, std::uint16_t* sizes,
                     std::uint8_t* packed, std::size_t used, SwapWrites writes, bool* wrote) noexcept;
bool UnitFill(std::uint8_t* to, std::uint32_t record, std::uint16_t size, const std::uint8_t* piece) noexcept;
// The 2 MiB at `at` (readable, and written by no one meanwhile) into the store: `units` gets the
// slot + 1 of each of its 64 KiB, SwapZeroUnit for those of zeroes. False when the slot stays as
// it is: it needs writes that `writes`, the budget or the store's room do not allow, or one failed
// (swapping then ends). pack_mutex held.
bool SwapOutSlot(std::uintptr_t at, std::uint32_t* units, SwapWrites writes) noexcept {
    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(at);
    std::uint64_t hashes[SwapUnits];
    std::size_t fresh = 0, found = 0, zeroes = 0;
    for (std::size_t unit = 0; unit < SwapUnits; ++unit) {
        const std::uint8_t* const here = bytes + unit * SwapUnit;
        if (SwapUnitZero(here)) {
            units[unit] = SwapZeroUnit;
            ++zeroes;
            continue;
        }
        hashes[unit] = SwapUnitHash(here);
        units[unit] = SwapFind(here, hashes[unit]);
        if (units[unit] != 0) {
            ++found;
            continue;
        }
        if (writes == SwapWrites::None) {
            swap_missed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ++fresh;
    }
    if (fresh != 0) {
        if (swap_count + fresh > swap_limit) {
            swap_full.fetch_add(1, std::memory_order_relaxed);
            swap_refused = true;
            return false;
        }
        if (!SwapBudget(fresh * SwapUnit, writes == SwapWrites::Urgent)) {
            swap_refused = true;
            return false;
        }
        // The new units go in one write, side by side in the slot's order. Each run of them used to
        // be a write of its own, and a write costs 5 ms whatever its size: late in an hour of Mario
        // Kart World, when the store has most units of a slot and the new ones lie scattered
        // between them, 971 MiB took 85 s to write (11 MiB a second, where whole slots go at 190),
        // with frames of 10 to 29 s while a load waited for them.
        std::uint64_t gathered[SwapUnits];
        std::uint8_t place[SwapUnits], twin[SwapUnits]; // of a new unit: its place in the write; the unit it repeats
        std::size_t count = 0;
        for (std::size_t unit = 0; unit < SwapUnits; ++unit) {
            twin[unit] = 0xFF;
            if (units[unit] != 0) continue;
            // Two units of this slot with the same bytes: the second is the first's slot.
            for (std::size_t earlier = 0; earlier < unit; ++earlier) {
                if (units[earlier] != 0 || twin[earlier] != 0xFF || hashes[earlier] != hashes[unit] ||
                    std::memcmp(bytes + earlier * SwapUnit, bytes + unit * SwapUnit, SwapUnit) != 0)
                    continue;
                twin[unit] = static_cast<std::uint8_t>(earlier);
                break;
            }
            if (twin[unit] != 0xFF) continue;
            place[unit] = static_cast<std::uint8_t>(count);
            gathered[count] = hashes[unit];
            std::memcpy(swap_gather + count * SwapUnit, bytes + unit * SwapUnit, SwapUnit);
            ++count;
        }
        const std::uint32_t first = SwapAppend(swap_gather, gathered, count);
        if (first == 0) {
            SwapOff("a write failed");
            return false;
        }
        for (std::size_t unit = 0; unit < SwapUnits; ++unit)
            if (units[unit] == 0 && twin[unit] == 0xFF) units[unit] = first + place[unit];
        for (std::size_t unit = 0; unit < SwapUnits; ++unit)
            if (twin[unit] != 0xFF) units[unit] = units[twin[unit]];
    }
    swap_found.fetch_add(found, std::memory_order_relaxed);
    swap_zero_units.fetch_add(zeroes, std::memory_order_relaxed);
    return true;
}

// A slot's 2 MiB from the store into `view` (a fault handler: no lock, the store's slots never
// change): false when they did not come back as they went.
bool SwapReadSlot(void* view, const std::uint32_t* units, std::uint64_t hash) noexcept {
    const std::uint64_t start = SwapNow();
    auto* const to = static_cast<std::uint8_t*>(view);
    bool ok = true;
    for (std::size_t unit = 0; unit < SwapUnits && ok;) {
        if (units[unit] == SwapZeroUnit) {
            std::memset(to + unit * SwapUnit, 0, SwapUnit);
            ++unit;
            continue;
        }
        // Units that went together lie in a row in the store: read in one call.
        std::size_t end = unit + 1;
        while (end < SwapUnits && units[end] != SwapZeroUnit && units[end] == units[end - 1] + 1) ++end;
        ok = units[unit] != 0 &&
             SwapReadAt(swap_store_fd, to + unit * SwapUnit, (end - unit) * SwapUnit, std::uint64_t{units[unit] - 1} * SwapUnit);
        unit = end;
    }
    ok = ok && FullHash(reinterpret_cast<std::uintptr_t>(view), SparseSlot) == hash;
    const std::uint64_t took = SwapNow() - start;
    swap_reads.fetch_add(1, std::memory_order_relaxed);
    swap_read_ns.fetch_add(took, std::memory_order_relaxed);
    std::uint64_t most = swap_read_max_ns.load(std::memory_order_relaxed);
    while (took > most && !swap_read_max_ns.compare_exchange_weak(most, took, std::memory_order_relaxed)) {}
    return ok;
}

// Per slot of the guest's range, the store's slot + 1 of each of its units while the store has its
// bytes (PackedSlot::swap; made with the packed table, under the slot's lock afterwards).
std::uint32_t* swap_units = nullptr;

// Forgets a slot's packed data, in the packed store or in the swap store (the slot is locked).
void UnitForget(std::size_t slot) noexcept;
void ForgetPacked(std::size_t slot) noexcept {
    PackedSlot* const table = packed_slots.load(std::memory_order_acquire);
    if (table == nullptr) return;
    if (table[slot].units) {
        UnitForget(slot);
        return;
    }
    if (table[slot].swap != 0) { // (its units stay where they are: the store is found by contents)
        swap_slots.fetch_sub(1, std::memory_order_relaxed);
    } else if (table[slot].data != nullptr) {
        packed_count.fetch_sub(1, std::memory_order_relaxed);
        packed_bytes.fetch_sub(table[slot].size, std::memory_order_relaxed);
        StoreRelease(table[slot].data, table[slot].size);
    }
    table[slot] = {};
}
// The swap store holds the slot's bytes (a Packed slot's only copy, or a Clean or Idle slot's second).
bool InSwap(std::size_t slot) noexcept {
    PackedSlot* const table = packed_slots.load(std::memory_order_acquire);
    return table != nullptr && table[slot].swap != 0;
}

#ifndef PS5_NATIVE
extern std::mutex pack_mutex;
// The host's check (below): the store as a session that has not started finds it.
void SwapCheckReset() noexcept {
    if (swap_store_fd >= 0) close(swap_store_fd);
    if (swap_index_fd >= 0) close(swap_index_fd);
    swap_store_fd = swap_index_fd = -1;
    swap_count = swap_limit = swap_cap = swap_carried = 0;
    swap_state.store(0, std::memory_order_release);
    swap_off_why.store("not needed yet", std::memory_order_release);
    for (auto* counter : {&swap_written, &swap_appended, &swap_found, &swap_zero_units}) counter->store(0, std::memory_order_relaxed);
}
} // namespace

// The host's check of the swap store's own parts (headless/memory_check.cpp): a slot there and back,
// its units found again without a write (in the same session and in the next), units of zeroes and
// twins, a changed byte in the store, the budget and its refill, the store's limit and the
// storage's, an index ahead of its store, and when the store starts over. `folder`: where the store
// may be made. Returns 0, or the number of the step that failed.
extern "C" int eden_swap_check(const char* folder) noexcept {
    char path[400], store[420], index[420];
    if (std::snprintf(path, sizeof(path), "%s/swap", folder) >= static_cast<int>(sizeof(path))) return 1;
    std::snprintf(store, sizeof(store), "%s/store.bin", path);
    std::snprintf(index, sizeof(index), "%s/index.bin", path);
    const auto session = [&](const char* limit_mib, const char* burst_mib, const char* start_over_hours = "0",
                             const char* rate_mib_hour = "0", const char* keep_free_mib = "0") noexcept {
        SwapCheckReset();
        setenv("EDEN_SWAP_START_OVER_HOURS", start_over_hours, 1);
        setenv("EDEN_SWAP_DIR", path, 1);
        setenv("EDEN_SWAP_MIB", limit_mib, 1);
        setenv("EDEN_SWAP_KEEP_FREE_MIB", keep_free_mib, 1);
        setenv("EDEN_SWAP_RATE_MIB_HOUR", rate_mib_hour, 1);
        setenv("EDEN_SWAP_BURST_MIB", burst_mib, 1);
        return SwapStart();
    };
    const std::lock_guard lock{pack_mutex};
    int failed = 0;
    auto expect = [&failed, step = 1](bool value) mutable noexcept {
        ++step;
        if (!value && failed == 0) failed = step;
        return value;
    };
    const auto file_size = [](const char* name) noexcept -> long long {
        struct stat status {};
        return stat(name, &status) == 0 ? static_cast<long long>(status.st_size) : -1;
    };
    // A slot of the guest's memory: unit 0 of zeroes, unit 5 the same bytes as unit 4, the rest random.
    std::vector<std::uint8_t> slot(SparseSlot), back(SparseSlot);
    std::uint64_t seed = 0x9e3779b97f4a7c15ull;
    for (std::uint8_t& byte : slot) byte = static_cast<std::uint8_t>((seed = seed * 6364136223846793005ull + 1442695040888963407ull) >> 56);
    std::memset(slot.data(), 0, SwapUnit);
    std::memcpy(slot.data() + 5 * SwapUnit, slot.data() + 4 * SwapUnit, SwapUnit);
    const auto at = reinterpret_cast<std::uintptr_t>(slot.data());
    const std::uint64_t whole = FullHash(at, SparseSlot);
    std::uint32_t units[SwapUnits], again[SwapUnits];

    // Session 1: an empty store, 8 MiB at once and no refill.
    if (!expect(session("64", "8"))) return failed;                                               // 2
    expect(swap_count == 0 && file_size(store) == 0 && file_size(index) == 0);                    // 3
    // Nothing of it is there yet: it does not go without a write, and goes with 30 of them.
    expect(!SwapOutSlot(at, units, SwapWrites::None) && swap_count == 0);                         // 4
    expect(SwapOutSlot(at, units, SwapWrites::Urgent) && swap_count == 30 && units[0] == SwapZeroUnit &&
           units[5] == units[4] && swap_written.load() == 30 * SwapUnit);                         // 5
    expect(file_size(store) == static_cast<long long>(30 * SwapUnit) && file_size(index) == 30 * 8);  // 6
    expect(SwapReadSlot(back.data(), units, whole) && std::memcmp(slot.data(), back.data(), SparseSlot) == 0); // 7
    // The same bytes again: found, nothing written, the same slots (the twin's too).
    expect(SwapOutSlot(at, again, SwapWrites::None) && swap_count == 30 && std::memcmp(units, again, sizeof(units)) == 0 &&
           swap_found.load() == 31);                                                              // 8
    // One unit changed: one slot more; the old unit's slot stays as it was.
    slot[7 * SwapUnit + 100] ^= 0xff;
    expect(SwapOutSlot(at, again, SwapWrites::Urgent) && swap_count == 31 && again[7] == 31 && again[8] == units[8]); // 9
    expect(SwapReadSlot(back.data(), again, FullHash(at, SparseSlot)) && std::memcmp(slot.data(), back.data(), SparseSlot) == 0); // 10
    slot[7 * SwapUnit + 100] ^= 0xff;
    expect(SwapReadSlot(back.data(), units, whole) && std::memcmp(slot.data(), back.data(), SparseSlot) == 0); // 11
    // The budget: 128 units, of which 31 are gone and a quarter (32) is kept for failed allocations.
    std::vector<std::uint8_t> other(SparseSlot);
    for (std::uint8_t& byte : other) byte = static_cast<std::uint8_t>((seed = seed * 6364136223846793005ull + 1442695040888963407ull) >> 56);
    const auto other_at = reinterpret_cast<std::uintptr_t>(other.data());
    const auto change = [&other]() noexcept {
        for (std::uint8_t& byte : other) byte = static_cast<std::uint8_t>(byte + 1);
    };
    expect(SwapOutSlot(other_at, again, SwapWrites::Budget) && swap_count == 63);                 // 12
    change();
    expect(SwapOutSlot(other_at, again, SwapWrites::Budget) && swap_count == 95);                 // 13
    change();
    expect(!SwapOutSlot(other_at, again, SwapWrites::Budget) && swap_count == 95 && swap_denied_rate.load() != 0); // 14
    expect(SwapOutSlot(other_at, again, SwapWrites::Urgent) && swap_count == 127);                // 15
    change();
    expect(!SwapOutSlot(other_at, again, SwapWrites::Urgent) && swap_count == 127);               // 16

    // Session 2 finds session 1's slots: the first slot goes again without a write.
    if (!expect(session("64", "8"))) return failed;                                               // 17
    expect(swap_count == 127 && swap_carried == 127);                                             // 18
    expect(SwapOutSlot(at, again, SwapWrites::None) && swap_count == 127 && std::memcmp(units, again, sizeof(units)) == 0 &&
           swap_written.load() == 0);                                                             // 19
    // A byte of the store changed behind its back: a slot read back with it is refused, and the
    // unit no longer passes for the slot's (it is appended anew).
    const std::uint8_t wrong = static_cast<std::uint8_t>(~slot[9 * SwapUnit + 10]);
    expect(pwrite(swap_store_fd, &wrong, 1, static_cast<off_t>(std::uint64_t{units[9] - 1} * SwapUnit + 10)) == 1); // 20
    swap_check_slots = 0; // (what was read to compare is from before the change)
    expect(!SwapReadSlot(back.data(), units, whole));                                             // 21
    expect(SwapOutSlot(at, again, SwapWrites::Urgent) && swap_count == 128 && again[9] == 128 && again[10] == units[10]); // 22
    expect(SwapReadSlot(back.data(), again, whole) && std::memcmp(slot.data(), back.data(), SparseSlot) == 0); // 23

    // Session 3: the store may hold 8 MiB and holds as much. What is there is found, nothing new goes.
    if (!expect(session("8", "64"))) return failed;                                               // 24
    expect(swap_count == 128 && swap_limit == 128 && swap_cap == 128);                            // 25
    expect(SwapOutSlot(at, again, SwapWrites::None));                                             // 26
    change();
    expect(!SwapOutSlot(other_at, again, SwapWrites::Urgent) && swap_count == 128 && swap_full.load() != 0); // 27

    // An index a power cut left ahead of its store (a hash more than the store has slots): the slot
    // does not count.
    SwapCheckReset();
    const std::uint64_t stray = 0x1234;
    const int fd = open(index, O_WRONLY);
    expect(fd >= 0 && pwrite(fd, &stray, sizeof(stray), 128 * 8) == sizeof(stray));                // 28
    if (fd >= 0) close(fd);
    if (!expect(session("64", "64"))) return failed;                                              // 29
    expect(swap_count == 128);                                                                    // 30
    // A store that has reached its size starts over, once a day at most; one with room left does not.
    char mark[420];
    std::snprintf(mark, sizeof(mark), "%s/started", path);
    // (`started`: set to so many seconds from now, and how far from now it is.)
    const auto set_started = [&](long long from_now) noexcept {
        const std::uint64_t then = static_cast<std::uint64_t>(static_cast<long long>(time(nullptr)) + from_now);
        const int file = open(mark, O_WRONLY);
        const bool done = file >= 0 && pwrite(file, &then, sizeof(then), 0) == sizeof(then);
        if (file >= 0) close(file);
        return done;
    };
    const auto started_from_now = [&]() noexcept -> long long {
        std::uint64_t then = 0;
        const int file = open(mark, O_RDONLY);
        const bool got = file >= 0 && pread(file, &then, sizeof(then), 0) == sizeof(then);
        if (file >= 0) close(file);
        return got ? static_cast<long long>(then) - static_cast<long long>(time(nullptr)) : 1ll << 40;
    };
    if (!expect(session("64", "64", "24"))) return failed;                                        // 31
    expect(swap_count == 128 && started_from_now() <= 0 && started_from_now() >= -5);             // 32 (its day starts with it)
    if (!expect(session("8", "64", "24"))) return failed;                                         // 33
    expect(swap_count == 128 && file_size(mark) == 8);                                            // 34 (its day is not over)
    expect(set_started(-25 * 3600));                                                              // 35
    if (!expect(session("8", "64", "24"))) return failed;                                         // 36
    expect(swap_count == 0 && file_size(store) == 0 && file_size(index) == 0);                    // 37
    expect(started_from_now() <= 0 && started_from_now() >= -5);                                  // 38 (and when is on the storage)
    expect(SwapOutSlot(at, again, SwapWrites::Urgent) && swap_count == 30 &&
           SwapReadSlot(back.data(), again, whole) && std::memcmp(slot.data(), back.data(), SparseSlot) == 0); // 39
    // Less than a quarter of its size left is what starts over: 30 slots of 64 stay, 30 of 32 do not.
    expect(set_started(-25 * 3600));                                                              // 40
    if (!expect(session("4", "64", "24"))) return failed;                                         // 41
    expect(swap_count == 30 && SwapOutSlot(at, again, SwapWrites::None));                         // 42
    if (!expect(session("2", "64", "24"))) return failed;                                         // 43
    expect(swap_count == 0 && file_size(store) == 0 && file_size(index) == 0);                    // 44
    // Never, with EDEN_SWAP_START_OVER_HOURS=0 (the store then takes only what it holds, as Lichii's does).
    expect(SwapOutSlot(at, again, SwapWrites::Urgent) && swap_count == 30 && set_started(-25 * 3600)); // 45
    if (!expect(session("2", "64", "0"))) return failed;                                          // 46
    expect(swap_count == 30 && SwapOutSlot(at, again, SwapWrites::None));                         // 47
    // Nor with a time from after now (a clock that was ahead, or is behind): the store waits, and the time stays.
    expect(set_started(3600));                                                                    // 48
    if (!expect(session("2", "64", "24"))) return failed;                                         // 49
    expect(swap_count == 30 && started_from_now() > 3500);                                        // 50
    // Nor without a time that can be written: `started` is a folder here.
    expect(unlink(mark) == 0 && mkdir(mark, 0777) == 0);                                          // 51
    if (!expect(session("2", "64", "24"))) return failed;                                         // 52
    expect(swap_count == 30 && rmdir(mark) == 0);                                                 // 53
    // A storage with nothing free beyond what is kept free: the store does not grow, and what it holds is found.
    if (!expect(session("64", "64", "0", "0", "1000000000"))) return failed;                      // 54
    const std::uint64_t full_before = swap_full.load();
    change();
    expect(swap_limit == 30 && SwapOutSlot(at, again, SwapWrites::None) &&
           !SwapOutSlot(other_at, again, SwapWrites::Urgent) && swap_count == 30 && swap_full.load() > full_before); // 55
    // The budget fills up again: 2 MiB at once, 10 MiB a second.
    if (!expect(session("64", "2", "0", "36000"))) return failed;                                 // 56
    change();
    const std::uint64_t spent_at = SwapNow();
    expect(SwapOutSlot(other_at, again, SwapWrites::Urgent) && swap_count == 62);                 // 57
    change();
    const bool at_once = SwapOutSlot(other_at, again, SwapWrites::Urgent);
    expect(!at_once || SwapNow() - spent_at >= 150'000'000ull);                                   // 58 (2 MiB more take 200 ms)
    usleep(400'000);
    change();
    expect(SwapOutSlot(other_at, again, SwapWrites::Urgent));                                     // 59
    // New units that do not lie side by side go in one write all the same, in the slot's order.
    if (!expect(session("64", "64"))) return failed;                                              // 60
    expect(SwapOutSlot(at, units, SwapWrites::None));                                             // 61
    for (const std::size_t unit : {std::size_t{3}, std::size_t{12}, std::size_t{25}}) slot[unit * SwapUnit + 7] ^= 0x55;
    const std::uint32_t count_before = swap_count;
    const std::uint64_t appends_before = swap_appends.load();
    expect(SwapOutSlot(at, again, SwapWrites::Urgent) && swap_count == count_before + 3 &&
           swap_appends.load() == appends_before + 1 && again[3] == count_before + 1 && again[12] == count_before + 2 &&
           again[25] == count_before + 3 && again[4] == units[4]);                                // 62
    expect(SwapReadSlot(back.data(), again, FullHash(at, SparseSlot)) && std::memcmp(slot.data(), back.data(), SparseSlot) == 0); // 63
    // A slot in units ("Units", below): zeroes keep nothing, what zstd brings to three quarters stays
    // in memory, what the store holds is found there, and the rest stays unless a write is allowed.
    // Every unit comes back as it went. (Only where zstd is linked in.)
    if (UnitZstdStart()) {
        std::vector<std::uint8_t> mixed(SparseSlot), packed(SparseSlot), unit(SwapUnit);
        for (std::uint8_t& byte : mixed) byte = static_cast<std::uint8_t>((seed = seed * 6364136223846793005ull + 1442695040888963407ull) >> 56);
        std::memset(mixed.data(), 0, SwapUnit);                                   // unit 0: zeroes
        for (std::size_t i = 0; i < SwapUnit; ++i) mixed[2 * SwapUnit + i] = static_cast<std::uint8_t>(i / 64); // unit 2: packs
        std::memcpy(mixed.data() + 3 * SwapUnit, slot.data() + 20 * SwapUnit, SwapUnit); // unit 3: the store has it
        std::uint16_t sizes[SwapUnits];
        bool wrote = false;
        const std::uint32_t held = swap_count;
        std::size_t used = UnitsOut(mixed.data(), 0xFFFFFFFFu, units, sizes, packed.data(), 0, SwapWrites::None, &wrote);
        bool stays = true;
        for (std::size_t i = 4; i < SwapUnits; ++i) stays = stays && units[i] == 0;
        expect(units[0] == UnitZero && units[2] != UnitZero && (units[2] & UnitPacked) && sizes[2] == used && used != 0 &&
               used <= UnitPackMax && units[3] == again[20] && units[1] == 0 && stays && !wrote && swap_count == held); // 64
        used = UnitsOut(mixed.data(), 0xFFFFFFFFu, units, sizes, packed.data(), 0, SwapWrites::Urgent, &wrote);
        bool same = wrote && swap_count == held + 29 && units[1] == held + 1 && units[31] == held + 29;
        for (std::size_t i = 0; i < SwapUnits && same; ++i)
            same = UnitFill(unit.data(), units[i], sizes[i], packed.data()) &&
                   std::memcmp(unit.data(), mixed.data() + i * SwapUnit, SwapUnit) == 0;
        expect(same);                                                                             // 65
        // A unit that is not what its record says is refused: a packed one cut short, one of the store changed.
        expect(!UnitFill(unit.data(), units[2], static_cast<std::uint16_t>(sizes[2] - 1), packed.data()) &&
               !UnitFill(unit.data(), UnitHere | 5, 0, packed.data()));                           // 66
        const std::uint8_t flipped = static_cast<std::uint8_t>(~mixed[SwapUnit + 9]);
        expect(pwrite(swap_store_fd, &flipped, 1, static_cast<off_t>(std::uint64_t{units[1] - 1} * SwapUnit + 9)) == 1 &&
               !UnitFill(unit.data(), units[1], 0, packed.data()));                               // 67
    } else {
        for (int skipped = 0; skipped < 4; ++skipped) expect(true);                               // 64-67
    }
    unlink(mark);
    SwapCheckReset();
    unlink(store);
    unlink(index);
    rmdir(path);
    for (const char* name : {"EDEN_SWAP_DIR", "EDEN_SWAP_MIB", "EDEN_SWAP_KEEP_FREE_MIB", "EDEN_SWAP_RATE_MIB_HOUR", "EDEN_SWAP_BURST_MIB",
                             "EDEN_SWAP_START_OVER_HOURS"})
        unsetenv(name);
    return failed;
}
namespace {
#endif

// ---- Idle slots: which slots nothing touches ----
// Packing used to choose slots by when they were last written (a sampled hash each pass). Reads are
// not seen that way: more than half of the slots packed in a Mario Kart World session were read and
// unpacked again (packs=1252 unpacks=724), a block and 2 MiB of LZ4 each time. For the swap store
// that would be a write each time. So while the pool is short the scanner takes all access away
// from slots for a while (Idle: the block stays, mprotect only). A slot the game or the emulator
// touches faults once and has its access back (a signal and an mprotect); it is then left alone
// for IdleBackoff seconds, twice as long each time it was woken or unpacked. A slot still Idle
// after IdleSeconds is one nothing reads or writes: those are packed, the longest idle first, and
// only those go to the swap store. EDEN_CRAM_IDLE=0 goes back to choosing by writes (and no swap).
bool IdleEnabled() noexcept {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_CRAM_IDLE");
        return value == nullptr || value[0] != '0';
    }();
    return on;
}
std::uint32_t* idle_since = nullptr;                    // per slot: when it was made Idle (s)
// Per slot: how often it was written again after it had stayed unwritten for RecycleQuietSeconds or
// more. That is memory the game reuses (a pool it loads the next track into): untouched for
// minutes, it looks like the best there is to take, and what is written to the store for it is
// thrown away at the game's next write. In a 14-minute Mario Kart World session 684 of 979 slots
// written to the store ended that way, whether they had been unwritten for one minute (79%) or for
// five to ten (61%): how long a slot has been unwritten does not tell the two kinds apart, its
// history does. Counted for free by the scanner, which sees a slot's sampled hash change
// (ColdPass), and by the write that drops a slot's copy in the store (MakeDirty, under the slot's
// lock; rewrite_noted keeps the scanner from counting that write again). Slots never counted go
// to the store first. The others go only when a request for memory would otherwise get nothing
// (RecoverMemory): they used to go once they had stayed unwritten for 2, 4, 8, then 16 minutes,
// and of the slots written that way after one and two such counts 74% and 83% were thrown away
// by the game's next write.
constexpr std::uint32_t RecycleQuietSeconds = 30;
std::atomic<std::uint8_t>* swap_rewritten = nullptr;
std::atomic<std::uint8_t>* rewrite_noted = nullptr;
void NoteRewritten(std::size_t slot) noexcept {
    if (swap_rewritten == nullptr) return;
    if (const auto times = swap_rewritten[slot].load(std::memory_order_relaxed); times < 255)
        swap_rewritten[slot].store(static_cast<std::uint8_t>(times + 1), std::memory_order_relaxed);
}
unsigned Rewritten(std::size_t slot) noexcept {
    return swap_rewritten != nullptr ? swap_rewritten[slot].load(std::memory_order_relaxed) : 0;
}
// What a slot was when it went to the swap store with a write (per slot: SwapAgeClass in bits 0-2,
// how often it had been written again in bits 4-5; bit 6 for one that went without a write, all of
// it being there), and how many of each kind were written and then dropped by a write of the game's
// (the report's EDEN_SWAP_AGES line).
std::uint8_t* swap_age_class = nullptr;
std::atomic<std::uint32_t> swap_age_written[6], swap_age_dropped[6], swap_kind_written[4], swap_kind_dropped[4];
unsigned SwapAgeClass(std::uint32_t unwritten_s) noexcept {
    return unwritten_s < 30 ? 0 : unwritten_s < 60 ? 1 : unwritten_s < 120 ? 2 : unwritten_s < 300 ? 3 : unwritten_s < 600 ? 4 : 5;
}
// A slot goes to the swap store (pack_mutex held): what it is then, and whether any of it was written.
void NoteSwapWrite(std::size_t slot, std::uint32_t unwritten_s, bool wrote) noexcept {
    if (swap_age_class == nullptr) return;
    const unsigned age = SwapAgeClass(unwritten_s), kind = std::min<unsigned>(Rewritten(slot), 3);
    swap_age_class[slot] = static_cast<std::uint8_t>(age | kind << 4 | (wrote ? 0 : 0x40));
    if (!wrote) return;
    swap_age_written[age].fetch_add(1, std::memory_order_relaxed);
    swap_kind_written[kind].fetch_add(1, std::memory_order_relaxed);
}
std::atomic<std::uint32_t>* touch_since = nullptr;      // per slot: when it was last woken or unpacked (s)
std::atomic<std::uint8_t>* touch_times = nullptr;       // per slot: how often
std::atomic<std::uint64_t> idled{0}, idle_woken{0};
// Read faults that found their slot Clean already (HandleGuestWriteFault), and those that then
// faulted again at once: the fault's write bit was wrong if these are many.
std::atomic<std::uint64_t> clean_late_reads{0}, clean_refaults{0};
std::uint32_t ColdNow() noexcept;
void NoteTouched(std::size_t slot) noexcept {
    if (touch_since == nullptr) return;
    touch_since[slot].store(ColdNow(), std::memory_order_relaxed);
    if (const auto times = touch_times[slot].load(std::memory_order_relaxed); times < 255)
        touch_times[slot].store(static_cast<std::uint8_t>(times + 1), std::memory_order_relaxed);
}
// Own or Clean -> Idle (the scanner; the slot is locked).
bool MakeIdle(SparseRange& range, std::size_t slot, std::uint32_t now) noexcept {
    DropSlotAliases(range, slot);
    if (mprotect(reinterpret_cast<void*>(SlotAddress(range, slot)), SparseSlot, PROT_NONE) != 0) return false;
    if (idle_since != nullptr) idle_since[slot] = now;
    idled.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// Idle -> Clean (the swap store has its bytes: read-only) or Own (a fault handler; the slot is locked).
std::uint8_t WakeIdle(SparseRange& range, std::size_t slot) noexcept {
    const bool clean = InSwap(slot);
    if (mprotect(reinterpret_cast<void*>(SlotAddress(range, slot)), SparseSlot, clean ? PROT_READ : PROT_READ | PROT_WRITE) != 0)
        std::abort();
    NoteTouched(slot);
    idle_woken.fetch_add(1, std::memory_order_relaxed);
    return clean ? SlotClean : SlotOwn;
}
// Clean or Idle -> Own for a write: read-write, and where the swap store has its bytes (as they
// were) is forgotten. The store keeps them: units the write leaves alone are found there again.
bool MakeDirty(SparseRange& range, std::size_t slot) noexcept {
    DropSlotAliases(range, slot); // read-only aliases of a Clean slot
    if (mprotect(reinterpret_cast<void*>(SlotAddress(range, slot)), SparseSlot, PROT_READ | PROT_WRITE) != 0) std::abort();
    if (InSwap(slot)) {
        ForgetPacked(slot);
        swap_dirtied.fetch_add(1, std::memory_order_relaxed);
        NoteRewritten(slot);
        if (rewrite_noted != nullptr) rewrite_noted[slot].store(1, std::memory_order_relaxed);
        if (const std::uint8_t was = swap_age_class != nullptr ? swap_age_class[slot] : 0x40; !(was & 0x40)) {
            swap_age_dropped[(was & 7) % 6].fetch_add(1, std::memory_order_relaxed);
            swap_kind_dropped[(was >> 4) & 3].fetch_add(1, std::memory_order_relaxed);
        }
    }
    return true;
}

// Blocks packing freed, kept for unpacking (KeepForUnpack below).
constexpr std::size_t UnpackKeepBlocks = 256;
std::int64_t unpack_keep[UnpackKeepBlocks];
std::atomic<std::size_t> unpack_keep_count{0};
std::atomic_flag unpack_keep_lock = ATOMIC_FLAG_INIT;
#ifdef PS5_NATIVE
// A zeroed block from the reserve (the pool refused one).
bool ReserveBlock(std::int64_t* physical) noexcept {
    Spin(reserve_lock);
    const std::size_t count = reserve_count.load(std::memory_order_relaxed);
    const bool ok = count > 0;
    if (ok) {
        *physical = reserve[count - 1];
        reserve_count.store(count - 1, std::memory_order_relaxed);
    }
    reserve_lock.clear(std::memory_order_release);
    if (ok) counters.reserve_used.fetch_add(1, std::memory_order_relaxed);
    return ok;
}
// Zeroed blocks into the reserve until it holds `blocks` (at most ReserveBlocks) or the pool says no.
std::atomic<std::uint64_t> reserve_hold_ns{0}; // no refill before this (CLOCK_MONOTONIC): GiveBackIdleBlocks
void FillReserve(std::size_t blocks = ReserveTarget()) noexcept {
    // What a request for memory was just given from here is left in the pool for its taker: refilled
    // 100 ms later, the same 94 MiB went back and forth thirty times while the heap's allocation,
    // which had asked for them, failed.
    if (SwapNow() < reserve_hold_ns.load(std::memory_order_relaxed)) return;
    blocks = std::min(blocks, ReserveTarget());
    for (;;) {
        Spin(reserve_lock);
        const bool full = reserve_count.load(std::memory_order_relaxed) >= blocks;
        reserve_lock.clear(std::memory_order_release);
        if (full) return;
        std::int64_t physical = -1;
        if (!ZeroedBlock(&physical)) return;
        Spin(reserve_lock);
        const std::size_t count = reserve_count.load(std::memory_order_relaxed);
        if (count < blocks) {
            reserve[count] = physical;
            reserve_count.store(count + 1, std::memory_order_relaxed);
            physical = -1;
        }
        reserve_lock.clear(std::memory_order_release);
        if (physical >= 0 && sceKernelReleaseDirectMemory(physical, SparseSlot) != 0) std::abort();
    }
}
// Blocks packing freed, kept for unpacking only (an unpack overwrites the whole block, so they are
// not zeroed). A burst of packing under a short pool is followed by reads of some of those slots: a
// read whose unpack found no block waited in its fault handler for up to 2 s, and with dozens of them
// the threads the GPU's work waits on stalled until the driver declared the device lost (Mario Kart
// World: every GPU hang came seconds after ~1.3 GB was packed). EDEN_CRAM_UNPACK_KEEP_MIB (default
// 256, at most 512) sets how many are kept.
std::size_t UnpackKeepTarget() noexcept {
    static const std::size_t blocks = [] {
        const char* value = std::getenv("EDEN_CRAM_UNPACK_KEEP_MIB");
        const std::size_t mib = value != nullptr ? std::strtoull(value, nullptr, 10) : 256;
        return std::min(UnpackKeepBlocks, mib / (SparseSlot >> 20));
    }();
    return blocks;
}
bool KeepForUnpack(std::int64_t physical) noexcept {
    Spin(unpack_keep_lock);
    const std::size_t count = unpack_keep_count.load(std::memory_order_relaxed);
    const bool kept = count < UnpackKeepTarget();
    if (kept) {
        unpack_keep[count] = physical;
        unpack_keep_count.store(count + 1, std::memory_order_relaxed);
    }
    unpack_keep_lock.clear(std::memory_order_release);
    return kept;
}
bool UnpackBlock(std::int64_t* physical) noexcept {
    Spin(unpack_keep_lock);
    const std::size_t count = unpack_keep_count.load(std::memory_order_relaxed);
    const bool ok = count > 0;
    if (ok) {
        *physical = unpack_keep[count - 1];
        unpack_keep_count.store(count - 1, std::memory_order_relaxed);
    }
    unpack_keep_lock.clear(std::memory_order_release);
    return ok;
}
void EmptyUnpackKeep() noexcept {
    Spin(unpack_keep_lock);
    for (std::size_t i = 0; i < unpack_keep_count.load(std::memory_order_relaxed); ++i)
        if (sceKernelReleaseDirectMemory(unpack_keep[i], SparseSlot) != 0) std::abort();
    unpack_keep_count.store(0, std::memory_order_relaxed);
    unpack_keep_lock.clear(std::memory_order_release);
}
void EmptyReserve() noexcept {
    EmptyUnpackKeep();
    Spin(reserve_lock);
    for (std::size_t i = 0; i < reserve_count.load(std::memory_order_relaxed); ++i)
        if (sceKernelReleaseDirectMemory(reserve[i], SparseSlot) != 0) std::abort();
    reserve_count.store(0, std::memory_order_relaxed);
    reserve_lock.clear(std::memory_order_release);
}
#endif

// One more slot shows `physical`; the first one makes it a shared block. False when the table is full.
bool ShareBlock(std::int64_t physical) noexcept {
    Spin(shared_lock);
    std::size_t free_entry = SharedBlocks;
    bool done = false;
    for (std::size_t i = 0; i < SharedBlocks && !done; ++i) {
        if (shared_refs[i] > 0 && shared_block[i] == physical) {
            ++shared_refs[i];
            done = true;
        } else if (shared_refs[i] == 0 && free_entry == SharedBlocks) {
            free_entry = i;
        }
    }
    if (!done && free_entry < SharedBlocks) {
        shared_block[free_entry] = physical;
        shared_refs[free_entry] = 1;
        done = true;
    }
    shared_lock.clear(std::memory_order_release);
    return done;
}
// One slot less shows `physical`: the references left (0: the block is no longer shown).
int UnshareBlock(std::int64_t physical) noexcept {
    Spin(shared_lock);
    int left = 0;
    for (std::size_t i = 0; i < SharedBlocks; ++i)
        if (shared_refs[i] > 0 && shared_block[i] == physical) {
            left = --shared_refs[i];
            break;
        }
    shared_lock.clear(std::memory_order_release);
    return left;
}

// ---- Units: a packed slot in 64 KiB units ----
// After PS5_Lichii's guest memory (mihawk, codeberg.org/mihawk/PS5_Lichii, src/common/host_memory.cpp:
// Ps5TrimZeroUnits, Ps5CommitOnTouch), which keeps all of the guest's memory in 64 KiB units, packs
// the cold ones with zstd, sends those that will not pack to the swap store, and brings one unit
// back at a touch. Here memory in use stays in 2 MiB blocks: the driver takes the pool in 2 MiB
// pieces, and a slot in use is one mapping. It is the slot nothing touches that goes in units:
//   - a unit of zeroes keeps nothing;
//   - a unit zstd brings to UnitPackMax or less stays in memory, in the slot's piece of the packed store;
//   - a unit whose bytes the swap store holds, or may be given (a request after a failed
//     allocation; EDEN_SWAP_PASS=2), is there;
//   - a unit that could only go with a write nobody asked for stays, in a unit of memory of its own.
// Such a slot is Packed and has no block, and its address space is a reservation. A touch brings
// back the unit touched and no other: 64 KiB of memory and one read, where a slot packed whole
// took 2 MiB and 32. That is what a touch costs in Lichii, and why its pass can send memory ahead
// of need: here the same pass ended Mario Kart World sessions while a touch brought 2 MiB back.
//
// Units of memory come from slabs, 2 MiB blocks of the pool cut in 32, so that what units give
// back returns to the pool in pieces the driver can take. A slab keeps a view of its own; units are
// filled and read through it. A unit with memory is mapped at its address (awake), or not mapped at
// all (idle): taking access away is one reservation over the whole slot, giving it back one mapping
// (Lichii's docs/ps5/kernel-calls.md: a map costs 2 us, a protection change 26 to 43). Units still
// idle after IdleSeconds go the way the slot's others went. A slot whose 32 units all have memory
// again is made whole (Own).
//
// With the 64-bit fastmem window (range.aliases) a touch brings the whole slot back, as before: the
// window shows a slot's block, and a slot in units has none.
// EDEN_CRAM_UNITS=0: slots are packed whole, as before (LZ4, or all of the slot in the swap store).
// A slot goes in units when that gives back this much; less, with units that stay, is not worth a
// slot in pieces.
constexpr std::size_t UnitMinGain = std::size_t{512} << 10;
struct UnitSlot {
    std::uint32_t here = 0;       // units with memory of their own (a slab unit; swap_units has which)
    std::uint32_t idle = 0;       // of those: not mapped now
    std::uint32_t idle_since = 0; // when its units were last made idle (ColdNow)
    std::uint32_t dead = 0;       // bytes of the slot's packed piece whose units came back
};
UnitSlot* unit_slots = nullptr;       // per slot (made with the packed table; the slot's lock)
std::uint16_t* unit_bytes = nullptr;  // per unit: its bytes in the slot's packed piece
std::uint8_t* unit_poor = nullptr;    // per slot: little of it would go without a write (until it is written)
std::atomic<std::uint64_t> unit_slots_made{0}, unit_backs{0}, unit_zero_reads{0}, unit_woken{0}, unit_evicted{0};
std::atomic<std::uint64_t> unit_moved{0}, unit_whole{0}, unit_packed{0}, unit_stayed{0}, unit_back_ns{0};

bool UnitsWanted() noexcept {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_CRAM_UNITS");
        return value == nullptr || value[0] != '0';
    }();
    return on && &ZSTD_createCCtx != nullptr && &ZSTD_createDCtx != nullptr && &ZSTD_compressCCtx != nullptr &&
           &ZSTD_decompressDCtx != nullptr && &ZSTD_isError != nullptr;
}
// One context to pack with (pack_mutex), eight to unpack with (fault handlers, which must not
// allocate: they are made here, once).
ZSTD_CCtx_s* unit_cctx = nullptr;
constexpr std::size_t UnitDctxs = 8;
std::atomic<ZSTD_DCtx_s*> unit_dctx[UnitDctxs];
std::atomic<bool> unit_zstd_ready{false};
bool UnitZstdStart() noexcept {
    if (unit_zstd_ready.load(std::memory_order_acquire)) return true;
    if (!UnitsWanted()) return false;
    if (unit_cctx == nullptr) unit_cctx = ZSTD_createCCtx();
    if (unit_cctx == nullptr) return false;
    for (auto& context : unit_dctx)
        if (context.load(std::memory_order_relaxed) == nullptr) {
            ZSTD_DCtx_s* const made = ZSTD_createDCtx();
            if (made == nullptr) return false;
            context.store(made, std::memory_order_release);
        }
    unit_zstd_ready.store(true, std::memory_order_release);
    return true;
}
bool UnitUnzstd(void* to, const void* from, std::size_t size) noexcept {
    for (;;) {
        for (auto& context : unit_dctx) {
            ZSTD_DCtx_s* const taken = context.exchange(nullptr, std::memory_order_acquire);
            if (taken == nullptr) continue;
            const std::size_t out = ZSTD_decompressDCtx(taken, to, SwapUnit, from, size);
            context.store(taken, std::memory_order_release);
            return out == SwapUnit;
        }
        sched_yield(); // all eight are unpacking
    }
}

// What each unit in `mask` of the 2 MiB at `bytes` becomes when it leaves (pack_mutex held): units[u]
// gets UnitZero, UnitPacked | its place in `packed` (sizes[u] bytes; `packed` is SparseSlot long and
// filled from `used` on), a slot of the swap store + 1, or 0 for a unit that stays: it would need a
// write that `writes`, the budget or the store's room do not allow. Returns the new `used`.
std::size_t UnitsOut(const std::uint8_t* bytes, std::uint32_t mask, std::uint32_t* units, std::uint16_t* sizes,
                     std::uint8_t* packed, std::size_t used, SwapWrites writes, bool* wrote) noexcept {
    std::uint64_t hashes[SwapUnits];
    std::uint32_t fresh = 0;
    std::size_t found = 0, zeroes = 0;
    const bool store = SwapOn();
    for (std::size_t unit = 0; unit < SwapUnits; ++unit) {
        if (!(mask >> unit & 1)) continue;
        const std::uint8_t* const here = bytes + unit * SwapUnit;
        units[unit] = 0;
        sizes[unit] = 0;
        if (SwapUnitZero(here)) {
            units[unit] = UnitZero;
            ++zeroes;
            continue;
        }
        // (A capacity of UnitPackMax: zstd gives up where the unit would be larger.)
        const std::size_t size = used + UnitPackMax <= SparseSlot && unit_cctx != nullptr
                                     ? ZSTD_compressCCtx(unit_cctx, packed + used, UnitPackMax, here, SwapUnit, 1) : 0;
        if (size != 0 && !ZSTD_isError(size) && size <= UnitPackMax) {
            units[unit] = UnitPacked | static_cast<std::uint32_t>(used);
            sizes[unit] = static_cast<std::uint16_t>(size);
            used += size;
            continue;
        }
        if (!store) continue;
        hashes[unit] = SwapUnitHash(here);
        units[unit] = SwapFind(here, hashes[unit]);
        if (units[unit] != 0) ++found;
        else if (writes != SwapWrites::None) fresh |= 1u << unit;
    }
    if (fresh != 0) {
        const std::size_t count = static_cast<std::size_t>(__builtin_popcount(fresh));
        if (swap_count + count > swap_limit) {
            swap_full.fetch_add(1, std::memory_order_relaxed);
            swap_refused = true;
        } else if (!SwapBudget(count * SwapUnit, writes == SwapWrites::Urgent)) {
            swap_refused = true;
        } else {
            // One write for them, side by side (SwapOutSlot has why).
            std::uint64_t gathered[SwapUnits];
            std::size_t next = 0;
            for (std::size_t unit = 0; unit < SwapUnits; ++unit)
                if (fresh >> unit & 1) {
                    gathered[next] = hashes[unit];
                    std::memcpy(swap_gather + next * SwapUnit, bytes + unit * SwapUnit, SwapUnit);
                    ++next;
                }
            const std::uint32_t first = SwapAppend(swap_gather, gathered, count);
            if (first == 0) {
                SwapOff("a write failed");
            } else {
                next = 0;
                for (std::size_t unit = 0; unit < SwapUnits; ++unit)
                    if (fresh >> unit & 1) units[unit] = first + static_cast<std::uint32_t>(next++);
                if (wrote != nullptr) *wrote = true;
            }
        }
    }
    swap_found.fetch_add(found, std::memory_order_relaxed);
    swap_zero_units.fetch_add(zeroes, std::memory_order_relaxed);
    return used;
}
// A unit's 64 KiB into `to`, from its record (not one with memory of its own): zeroes, its bytes in
// the slot's packed piece, or its slot of the swap store, checked against the hash the store's
// index has for it. A fault handler: no lock, nothing allocated. False when they did not come back
// as they went.
bool UnitFill(std::uint8_t* to, std::uint32_t record, std::uint16_t size, const std::uint8_t* piece) noexcept {
    if (record == UnitZero) {
        std::memset(to, 0, SwapUnit);
        return true;
    }
    if (record & UnitPacked) return piece != nullptr && UnitUnzstd(to, piece + (record & UnitIndex), size);
    if (record == 0 || record > SwapMaxSlots) return false;
    return SwapReadAt(swap_store_fd, to, SwapUnit, std::uint64_t{record - 1} * SwapUnit) &&
           SwapUnitHash(to) == swap_hashes[record - 1];
}

#ifdef PS5_NATIVE
// Slabs: blocks of the pool cut into units. slab i is seen at slab_base + i * 2 MiB by the service
// alone, for as long as it has a unit taken.
constexpr std::size_t UnitSlabs = 4096;
std::uintptr_t slab_base = 0;
std::int64_t slab_physical[UnitSlabs];
std::uint32_t slab_used[UnitSlabs];
std::uint32_t* slab_owner = nullptr; // per unit of a slab: slot * SwapUnits + unit + 1 of who has it
std::atomic_flag slab_lock = ATOMIC_FLAG_INIT;
std::atomic<std::size_t> slab_held{0}, slab_units{0};
// (pack_mutex held; not in a fault handler)
bool SlabStart() noexcept {
    if (slab_base != 0) return true;
    if (slab_owner == nullptr) slab_owner = new (std::nothrow) std::uint32_t[UnitSlabs * SwapUnits]();
    if (slab_owner == nullptr) return false;
    void* address = reinterpret_cast<void*>(cpu_mapping_hint);
    if (sceKernelReserveVirtualRange(&address, UnitSlabs * SparseSlot, 0, SparseSlot) != 0) return false;
    for (std::size_t i = 0; i < UnitSlabs; ++i) slab_physical[i] = -1, slab_used[i] = 0;
    slab_base = reinterpret_cast<std::uintptr_t>(address);
    return true;
}
std::uint8_t* UnitView(std::uint32_t index) noexcept {
    return reinterpret_cast<std::uint8_t*>(slab_base + std::size_t{index} * SwapUnit);
}
std::int64_t UnitPhysical(std::uint32_t index) noexcept {
    return slab_physical[index / SwapUnits] + static_cast<std::int64_t>((index % SwapUnits) * SwapUnit);
}
// A unit of memory: from the first slab with room, else from a new one. `below`: only from slabs
// before that one, and no new slab (SlabCompact). False when the pool has no block.
bool UnitTake(std::uint32_t* index, std::size_t below = UnitSlabs) noexcept {
    Spin(slab_lock);
    std::size_t empty = UnitSlabs;
    for (std::size_t i = 0; i < below; ++i) {
        if (slab_physical[i] < 0) {
            if (empty == UnitSlabs) empty = i;
            continue;
        }
        if (slab_used[i] == 0xFFFFFFFFu) continue;
        const unsigned bit = static_cast<unsigned>(__builtin_ctz(~slab_used[i]));
        slab_used[i] |= 1u << bit;
        slab_lock.clear(std::memory_order_release);
        *index = static_cast<std::uint32_t>(i * SwapUnits + bit);
        slab_units.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    std::int64_t physical = -1;
    if (below != UnitSlabs || empty == UnitSlabs ||
        (eden_direct_low(SparseSlot, SparseSlot, 12, &physical) != 0 && !UnpackBlock(&physical) && !ReserveBlock(&physical))) {
        slab_lock.clear(std::memory_order_release);
        return false;
    }
    if (!MapSlot(slab_base + empty * SparseSlot, PROT_READ | PROT_WRITE, physical)) {
        slab_lock.clear(std::memory_order_release);
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    slab_physical[empty] = physical;
    slab_used[empty] = 1;
    slab_lock.clear(std::memory_order_release);
    *index = static_cast<std::uint32_t>(empty * SwapUnits);
    slab_held.fetch_add(SparseSlot, std::memory_order_relaxed);
    slab_units.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// A unit of memory goes back (it is mapped at no address any more). A slab with none taken goes back
// to the pool. Returns whether one did.
bool UnitGive(std::uint32_t index) noexcept {
    const std::size_t slab = index / SwapUnits;
    std::int64_t gone = -1;
    Spin(slab_lock);
    slab_used[slab] &= ~(1u << (index % SwapUnits));
    slab_owner[index] = 0;
    // (Its view first: memory that went back must not stay in reach.)
    if (slab_used[slab] == 0 && MapSlot(slab_base + slab * SparseSlot, PROT_NONE, zero_block))
        gone = std::exchange(slab_physical[slab], -1);
    slab_lock.clear(std::memory_order_release);
    slab_units.fetch_sub(1, std::memory_order_relaxed);
    if (gone < 0) return false;
    if (sceKernelReleaseDirectMemory(gone, SparseSlot) != 0) std::abort();
    slab_held.fetch_sub(SparseSlot, std::memory_order_relaxed);
    return true;
}
bool MapUnit(std::uintptr_t at, int protection, std::int64_t physical) noexcept {
    void* address = reinterpret_cast<void*>(at);
    return sceKernelMapDirectMemory(&address, SwapUnit, protection, MAP_FIXED, physical, FinePage) == 0 &&
           address == reinterpret_cast<void*>(at);
}
#endif

// A slot in units gives up all it holds: its units of memory and its packed piece (the slot is
// locked, and its units are mapped nowhere any more, or its range is going).
void UnitForget(std::size_t slot) noexcept {
    PackedSlot* const table = packed_slots.load(std::memory_order_acquire);
    if (table == nullptr || unit_slots == nullptr || swap_units == nullptr) return;
#ifdef PS5_NATIVE
    UnitSlot& state = unit_slots[slot];
    for (std::size_t unit = 0; unit < SwapUnits; ++unit)
        if (state.here >> unit & 1) (void)UnitGive(swap_units[slot * SwapUnits + unit] & UnitIndex);
    guest_committed.fetch_sub(static_cast<std::size_t>(__builtin_popcount(state.here)) * SwapUnit, std::memory_order_relaxed);
    state = {};
#endif
    if (table[slot].data != nullptr) {
        packed_bytes.fetch_sub(table[slot].size, std::memory_order_relaxed);
        StoreRelease(table[slot].data, table[slot].size);
    }
    packed_count.fetch_sub(1, std::memory_order_relaxed);
    std::memset(swap_units + slot * SwapUnits, 0, SwapUnits * sizeof(std::uint32_t));
    table[slot] = {};
}

#ifdef PS5_NATIVE
bool UnpackSlot(SparseRange& range, std::size_t slot, std::uint8_t* to, bool for_write) noexcept;
// A touch of a slot in units, at `address` (a fault handler; the slot is locked): the unit touched
// gets its memory back, and its access. *to: the slot's state afterwards. *shown_read_only: it was a
// read of a unit of zeroes, which shows the zero block now; a write there faults once more. False
// when the pool had no block for a new slab (the caller waits and asks again).
bool UnitBack(SparseRange& range, std::size_t slot, std::uintptr_t address, bool write, std::uint8_t* to,
              bool* shown_read_only) noexcept {
    PackedSlot* const table = packed_slots.load(std::memory_order_acquire);
    *to = SlotPacked;
    // The fastmem window shows whole blocks: once it is in use, the slot comes back whole. (The
    // range's table of aliases is no sign of it: every guest range has one.)
    if (window_seen.load(std::memory_order_relaxed)) return UnpackSlot(range, slot, to, false);
    const std::uint64_t start = SwapNow();
    const std::uintptr_t at = SlotAddress(range, slot);
    const std::size_t unit = (address - at) / SwapUnit;
    const std::uintptr_t unit_at = at + unit * SwapUnit;
    const std::uint32_t bit = 1u << unit;
    UnitSlot& state = unit_slots[slot];
    std::uint32_t& record = swap_units[slot * SwapUnits + unit];
    if (state.here & bit) {
        // It has its memory: mapped again if it was idle (else another thread came first).
        if (state.idle & bit) {
            if (!MapUnit(unit_at, PROT_READ | PROT_WRITE, UnitPhysical(record & UnitIndex))) std::abort();
            state.idle &= ~bit;
            if (unit_poor != nullptr) unit_poor[slot] = 0; // it may be written now
            NoteTouched(slot);
            unit_woken.fetch_add(1, std::memory_order_relaxed);
            // (All 32 have memory and the block for the whole slot was not to be had then.)
            if (state.here == 0xFFFFFFFFu) (void)UnpackSlot(range, slot, to, true);
        }
        return true;
    }
    // Zeroes that are only read are the zero block's, as a slot of zeroes is.
    if (record == UnitZero && !write) {
        if (!MapUnit(unit_at, PROT_READ, zero_block)) std::abort();
        unit_zero_reads.fetch_add(1, std::memory_order_relaxed);
        *shown_read_only = true;
        return true;
    }
    std::uint32_t index = 0;
    if (!UnitTake(&index)) return false;
    if (!UnitFill(UnitView(index), record, unit_bytes[slot * SwapUnits + unit], static_cast<const std::uint8_t*>(table[slot].data))) {
        Note("EDEN_CRAM a unit's data did not come back as it went\n");
        std::abort(); // the guest's data is gone: never carry on
    }
    if (!MapUnit(unit_at, PROT_READ | PROT_WRITE, UnitPhysical(index))) {
        (void)UnitGive(index);
        return false;
    }
    if (record != UnitZero && (record & UnitPacked)) {
        // Its bytes in the slot's piece are dead; the piece goes when all of it is.
        state.dead += unit_bytes[slot * SwapUnits + unit];
        if (state.dead >= table[slot].size) {
            packed_bytes.fetch_sub(table[slot].size, std::memory_order_relaxed);
            StoreRelease(table[slot].data, table[slot].size);
            table[slot].data = nullptr;
            table[slot].size = 0;
            state.dead = 0;
        }
    }
    unit_bytes[slot * SwapUnits + unit] = 0;
    record = UnitHere | index;
    slab_owner[index] = static_cast<std::uint32_t>(slot * SwapUnits + unit + 1);
    state.here |= bit;
    guest_committed.fetch_add(SwapUnit, std::memory_order_relaxed);
    NoteTouched(slot);
    unit_backs.fetch_add(1, std::memory_order_relaxed);
    unit_back_ns.fetch_add(SwapNow() - start, std::memory_order_relaxed);
    // All of it has memory again: one block, and one mapping (a copy, nothing is read).
    if (state.here == 0xFFFFFFFFu) (void)UnpackSlot(range, slot, to, true);
    return true;
}

// A slot in units -> Own: a block of its own with every unit's bytes in it (the slot is locked).
bool UnpackUnits(SparseRange& range, std::size_t slot, std::uint8_t* to) noexcept {
    PackedSlot* const table = packed_slots.load(std::memory_order_acquire);
    const std::uintptr_t at = SlotAddress(range, slot);
    UnitSlot& state = unit_slots[slot];
    std::int64_t physical = -1;
    if (eden_direct_low(SparseSlot, SparseSlot, 12, &physical) != 0 && !UnpackBlock(&physical) && !ReserveBlock(&physical))
        return false;
    void* const view = MapView(physical, SparseSlot, PROT_READ | PROT_WRITE, SparseSlot);
    if (view == nullptr) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    // No access to any of it while its units are gathered: a write to one that is mapped would be
    // lost. One reservation over the slot; the units keep their memory, read through the slabs' views.
    const std::uint32_t awake = state.here & ~state.idle;
    if (ReserveOver(reinterpret_cast<void*>(at), SparseSlot, SparseSlot) != 1) std::abort();
    state.idle = state.here;
    for (std::size_t unit = 0; unit < SwapUnits; ++unit) {
        auto* const out = static_cast<std::uint8_t*>(view) + unit * SwapUnit;
        const std::uint32_t record = swap_units[slot * SwapUnits + unit];
        if (state.here >> unit & 1) {
            std::memcpy(out, UnitView(record & UnitIndex), SwapUnit);
        } else if (!UnitFill(out, record, unit_bytes[slot * SwapUnits + unit], static_cast<const std::uint8_t*>(table[slot].data))) {
            Note("EDEN_CRAM a unit's data did not come back as it went\n");
            std::abort(); // the guest's data is gone: never carry on
        }
    }
    UnmapView(view, SparseSlot);
    if (!MapSlot(at, PROT_READ | PROT_WRITE, physical)) {
        // The slot stays in units; those that were mapped are mapped again.
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        for (std::size_t unit = 0; unit < SwapUnits; ++unit)
            if (awake >> unit & 1) {
                if (!MapUnit(at + unit * SwapUnit, PROT_READ | PROT_WRITE, UnitPhysical(swap_units[slot * SwapUnits + unit] & UnitIndex)))
                    std::abort();
                state.idle &= ~(1u << unit);
            }
        return false;
    }
    UnitForget(slot);
    range.physical[slot] = physical;
    NoteTouched(slot);
    guest_committed.fetch_add(SparseSlot, std::memory_order_relaxed);
    unpacks.fetch_add(1, std::memory_order_relaxed);
    unit_whole.fetch_add(1, std::memory_order_relaxed);
    *to = SlotOwn;
    return true;
}
#endif

// ---- The changes of a slot (the slot is locked) ----

// Zero -> Own: a zeroed block of its own, read-write.
bool PromoteZero(SparseRange& range, std::size_t slot) noexcept {
    DropSlotAliases(range, slot);
    const std::uintptr_t at = SlotAddress(range, slot);
#ifdef PS5_NATIVE
    std::int64_t physical = -1;
    if (!ZeroedBlock(&physical) && !ReserveBlock(&physical)) return false;
    if (!MapSlot(at, PROT_READ | PROT_WRITE, physical)) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    range.physical[slot] = physical;
#else
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ | PROT_WRITE) != 0) return false;
    range.physical[slot] = 0;
#endif
    guest_committed.fetch_add(SparseSlot, std::memory_order_relaxed);
    return true;
}

// Shared -> Own: the last slot to show a block takes it back read-write; the others copy it.
bool PromoteShared(SparseRange& range, std::size_t slot) noexcept {
    DropSlotAliases(range, slot);
    const std::uintptr_t at = SlotAddress(range, slot);
#ifdef PS5_NATIVE
    const std::int64_t shared = range.physical[slot];
    Spin(shared_lock);
    bool last = false;
    for (std::size_t i = 0; i < SharedBlocks; ++i)
        if (shared_refs[i] > 0 && shared_block[i] == shared) {
            last = shared_refs[i] == 1;
            if (last) shared_refs[i] = 0;
            break;
        }
    shared_lock.clear(std::memory_order_release);
    if (last) return mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ | PROT_WRITE) == 0;
    std::int64_t physical = -1;
    if (eden_direct_low(SparseSlot, SparseSlot, 12, &physical) != 0 && !ReserveBlock(&physical)) return false;
    void* const view = MapView(physical, SparseSlot, PROT_READ | PROT_WRITE, SparseSlot);
    if (view == nullptr) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    std::memcpy(view, reinterpret_cast<const void*>(at), SparseSlot);
    UnmapView(view, SparseSlot);
    if (!MapSlot(at, PROT_READ | PROT_WRITE, physical)) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    range.physical[slot] = physical;
    if (UnshareBlock(shared) == 0 && sceKernelReleaseDirectMemory(shared, SparseSlot) != 0) std::abort();
    guest_committed.fetch_add(SparseSlot, std::memory_order_relaxed);
    counters.copies.fetch_add(1, std::memory_order_relaxed);
    return true;
#else
    (void)range;
    (void)slot;
    (void)at;
    return false; // the host never shares
#endif
}

// Own, Shared or Fine -> Zero: the slot shows the zero block again and its memory goes back.
void ReturnSlot(SparseRange& range, std::size_t slot) noexcept {
    DropSlotAliases(range, slot);
    const std::uintptr_t at = SlotAddress(range, slot);
    const std::uint8_t state = SlotState(range, slot);
    if (state == SlotPacked) {
        // Its block went back when it was packed: it shows the zero block (readable) again.
#ifdef PS5_NATIVE
        if (!MapSlot(at, PROT_READ, zero_block)) std::abort();
#endif
        ForgetPacked(slot);
        range.physical[slot] = -1;
        return;
    }
    // Idle or Clean: a block of its own, as an Own slot's (below), and maybe a copy in the swap store.
    if (state == SlotIdle || state == SlotClean) ForgetPacked(slot);
    if (state == SlotFine) {
        const std::size_t kept = FineKept(range, slot);
#ifdef PS5_NATIVE
        if (!MapSlot(at, PROT_READ, zero_block)) std::abort();
        if (kept != 0 && sceKernelReleaseDirectMemory(range.physical[slot], kept * FinePage) != 0) std::abort();
#else
        if (madvise(reinterpret_cast<void*>(at), SparseSlot, MADV_DONTNEED) != 0) std::abort();
#endif
        guest_committed.fetch_sub(kept * FinePage, std::memory_order_relaxed);
        range.fine[slot] &= 0xf00;
        range.physical[slot] = -1;
        return;
    }
#ifdef PS5_NATIVE
    if (!MapSlot(at, PROT_READ, zero_block)) std::abort();
    if (state == SlotShared) {
        if (UnshareBlock(range.physical[slot]) == 0) {
            if (sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
            guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
        }
    } else {
        if (sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
        guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
    }
#else
    (void)state;
    if (madvise(reinterpret_cast<void*>(at), SparseSlot, MADV_DONTNEED) != 0 ||
        mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ) != 0)
        std::abort();
    guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
#endif
    range.physical[slot] = -1;
}

// Packed -> Own: a block of its own, read-write, with the slot's data unpacked into it. From the swap
// store -> Clean instead (read-only, the store keeps its copy), unless the block is wanted for writing.
// *to: the slot's state afterwards.
bool UnpackSlot(SparseRange& range, std::size_t slot, std::uint8_t* to, bool for_write = false) noexcept {
#ifdef PS5_NATIVE
    PackedSlot* const table = packed_slots.load(std::memory_order_acquire);
    if (table != nullptr && table[slot].units) return UnpackUnits(range, slot, to);
    if (table == nullptr || (table[slot].data == nullptr && table[slot].swap == 0)) std::abort();
    const bool from_swap = table[slot].swap != 0;
    const std::uintptr_t at = SlotAddress(range, slot);
    std::int64_t physical = -1;
    if (eden_direct_low(SparseSlot, SparseSlot, 12, &physical) != 0 && !UnpackBlock(&physical) &&
        !ReserveBlock(&physical))
        return false;
    // Unpacked in a view of its own and only then mapped at the slot: mapped first, a reader that
    // had not faulted yet would see the block half unpacked.
    void* const view = MapView(physical, SparseSlot, PROT_READ | PROT_WRITE, SparseSlot);
    if (view == nullptr) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    if (from_swap) {
        if (swap_units == nullptr || !SwapReadSlot(view, swap_units + slot * SwapUnits, table[slot].hash)) {
            Note("EDEN_SWAP a slot's data did not come back from the swap store as it was written\n");
            std::abort(); // the guest's data is gone: never carry on
        }
    } else {
        const int out = LZ4_decompress_safe(static_cast<const char*>(table[slot].data), static_cast<char*>(view),
                                            static_cast<int>(table[slot].size), static_cast<int>(SparseSlot));
        if (out != static_cast<int>(SparseSlot)) std::abort(); // the guest's data is gone: never carry on
    }
    if (pack_kept != nullptr && pack_kept[slot] >= 0) {
        if (void* const kept_view = MapView(pack_kept[slot], SparseSlot, PROT_READ, SparseSlot); kept_view != nullptr) {
            pack_keep_compared.fetch_add(1, std::memory_order_relaxed);
            const auto* a = static_cast<const std::uint64_t*>(view);
            const auto* b = static_cast<const std::uint64_t*>(kept_view);
            for (std::size_t i = 0; i < SparseSlot / 8; ++i)
                if (a[i] != b[i]) {
                    if (pack_keep_differed.fetch_add(1, std::memory_order_relaxed) < 8) {
                        char line[200];
                        const int n = std::snprintf(line, sizeof(line),
                            "EDEN_CRAM_PACK_KEEP slot %zu changed while packed: word %zu unpacked %#llx kept %#llx\n",
                            slot, i, (unsigned long long)a[i], (unsigned long long)b[i]);
                        if (n > 0) Note(line);
                    }
                    break;
                }
            UnmapView(kept_view, SparseSlot);
        }
        (void)sceKernelReleaseDirectMemory(pack_kept[slot], SparseSlot);
        pack_kept[slot] = -1;
    }
    UnmapView(view, SparseSlot);
    const bool clean = from_swap && !for_write;
    if (!MapSlot(at, clean ? PROT_READ : PROT_READ | PROT_WRITE, physical)) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    range.physical[slot] = physical;
    if (!clean) ForgetPacked(slot); // a Clean slot's entry stays: the store keeps the copy
    NoteTouched(slot);
    guest_committed.fetch_add(SparseSlot, std::memory_order_relaxed);
    unpacks.fetch_add(1, std::memory_order_relaxed);
    *to = clean ? SlotClean : SlotOwn;
    return true;
#else
    (void)range;
    (void)slot;
    (void)to;
    (void)for_write;
    return false; // the host never packs
#endif
}

bool AllZero(std::uintptr_t at) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    for (std::size_t i = 0; i < SparseSlot / 8; i += 8) {
        if (word[i] | word[i + 1] | word[i + 2] | word[i + 3] | word[i + 4] | word[i + 5] | word[i + 6] | word[i + 7])
            return false;
    }
    return true;
}

bool PageZero(std::uintptr_t at) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    for (std::size_t i = 0; i < FinePage / 8; i += 8)
        if (word[i] | word[i + 1] | word[i + 2] | word[i + 3] | word[i + 4] | word[i + 5] | word[i + 6] | word[i + 7])
            return false;
    return true;
}

// Fine -> Own: a written Fine slot becomes one block of 2 MiB again (a copy of what it shows), and
// stays whole for the rest of the session.
bool PromoteFine(SparseRange& range, std::size_t slot) noexcept {
    DropSlotAliases(range, slot);
    const std::uintptr_t at = SlotAddress(range, slot);
    const std::size_t kept = FineKept(range, slot);
#ifdef PS5_NATIVE
    std::int64_t physical = -1;
    if (eden_direct_low(SparseSlot, SparseSlot, 12, &physical) != 0 && !ReserveBlock(&physical)) return false;
    void* const view = MapView(physical, SparseSlot, PROT_READ | PROT_WRITE, SparseSlot);
    if (view == nullptr) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    std::memcpy(view, reinterpret_cast<const void*>(at), SparseSlot);
    UnmapView(view, SparseSlot);
    if (!MapSlot(at, PROT_READ | PROT_WRITE, physical)) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    if (kept != 0 && sceKernelReleaseDirectMemory(range.physical[slot], kept * FinePage) != 0) std::abort();
    range.physical[slot] = physical;
#else
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ | PROT_WRITE) != 0) return false;
#endif
    guest_committed.fetch_add(SparseSlot - kept * FinePage, std::memory_order_relaxed);
    const unsigned times = std::min<unsigned>(((range.fine[slot] >> 8) & 0xf) + 1, FineBackoffMax);
    range.fine[slot] = static_cast<std::uint16_t>(times << 8);
    range.fine_after[slot] = static_cast<std::uint32_t>(counters.passes.load(std::memory_order_relaxed)) + (1u << times);
    counters.unfined.fetch_add(1, std::memory_order_relaxed);
    return true;
}

#ifdef PS5_NATIVE
bool MapRun(std::uintptr_t at, std::size_t bytes, std::int64_t physical) noexcept {
    void* address = reinterpret_cast<void*>(at);
    return sceKernelMapDirectMemory(&address, bytes, PROT_READ, MAP_FIXED, physical, FinePage) == 0 &&
           address == reinterpret_cast<void*>(at);
}
#endif

// Own -> Fine (the slot is locked and fenced, and has `zero` marked in a mask of its pages): its other
// pages go into a compact block, and the slot is mapped again page run by page run, read-only.
// The 2 MiB mapping is taken away whole first (a large mapping is never split); a thread that
// touches the slot meanwhile faults, waits for the lock and repeats its access.
bool MakeFine(SparseRange& range, std::size_t slot, const bool (&zero)[FinePages], std::size_t kept) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
#ifdef PS5_NATIVE
    std::int64_t compact = -1;
    if (eden_direct_low(kept * FinePage, FinePage, 12, &compact) != 0) return false;
    void* const view = MapView(compact, kept * FinePage, PROT_READ | PROT_WRITE, FinePage);
    if (view == nullptr) {
        (void)sceKernelReleaseDirectMemory(compact, kept * FinePage);
        return false;
    }
    std::size_t next = 0;
    for (std::size_t page = 0; page < FinePages; ++page)
        if (!zero[page]) {
            std::memcpy(static_cast<std::uint8_t*>(view) + next * FinePage,
                        reinterpret_cast<const void*>(at + page * FinePage), FinePage);
            ++next;
        }
    UnmapView(view, kept * FinePage);
    const std::int64_t whole = range.physical[slot];
    if (munmap(reinterpret_cast<void*>(at), SparseSlot) != 0) std::abort();
    bool ok = true;
    next = 0;
    for (std::size_t page = 0; page < FinePages && ok;) {
        std::size_t end = page + 1;
        while (end < FinePages && zero[end] == zero[page]) ++end;
        const std::size_t bytes = (end - page) * FinePage;
        ok = MapRun(at + page * FinePage, bytes, zero[page] ? zero_block : compact + std::int64_t(next * FinePage));
        if (!zero[page]) next += end - page;
        page = end;
    }
    if (!ok) {
        // The slot shows its block again, as it was (fenced: read-only until the caller unfences).
        if (!MapSlot(at, PROT_READ, whole)) std::abort();
        (void)sceKernelReleaseDirectMemory(compact, kept * FinePage);
        return false;
    }
    if (sceKernelReleaseDirectMemory(whole, SparseSlot) != 0) std::abort();
    range.physical[slot] = compact;
#else
    for (std::size_t page = 0; page < FinePages; ++page)
        if (zero[page] && madvise(reinterpret_cast<void*>(at + page * FinePage), FinePage, MADV_DONTNEED) != 0)
            std::abort();
#endif
    guest_committed.fetch_sub(SparseSlot - kept * FinePage, std::memory_order_relaxed);
    range.fine[slot] = static_cast<std::uint16_t>((range.fine[slot] & 0xf00) | kept);
    counters.fined.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// CommitSparsePage for a range with slot states.
bool ServiceCommit(SparseRange& range, std::size_t slot) noexcept {
    LockSlot(range, slot);
    const std::uint8_t state = SlotState(range, slot);
    // The block is wanted for writing: an Idle or Clean slot is Own again, without its copy in the swap store.
    std::uint8_t to = SlotOwn;
    const bool ok = state == SlotZero ? PromoteZero(range, slot)
                  : state == SlotFine ? PromoteFine(range, slot)
                  : state == SlotPacked ? UnpackSlot(range, slot, &to, true)
                  : state == SlotIdle || state == SlotClean ? MakeDirty(range, slot) : true;
    UnlockSlot(range, slot,
               ok && (state == SlotZero || state == SlotFine || state == SlotPacked || state == SlotIdle || state == SlotClean)
                   ? SlotOwn : state);
    return ok;
}

// ---- The fault ----
SparseRange* WriteRangeOf(std::uintptr_t address) noexcept {
    for (SparseRange* range : {service_range.load(std::memory_order_acquire), probe_range.load(std::memory_order_acquire)}) {
        if (range == nullptr || range->state == nullptr) continue;
        const auto begin = range->begin.load(std::memory_order_acquire);
        if (begin != 0 && address >= begin && address < range->end) return range;
    }
    return nullptr;
}
std::atomic<std::uintptr_t> probe_expected{0}; // the self-test's write, whatever si_addr says
std::atomic<bool> probe_mismatch{false};
// The last write the service refused, for the crash report: the address, then why (1: it faulted
// again and again with nothing to change, 2: no block was left for its slot) and the slot's state.
std::atomic<std::uintptr_t> refused_address{0};
std::atomic<unsigned> refused_detail{0}; // why | state << 8
void Refused(std::uintptr_t address, unsigned why, std::uint8_t state) noexcept {
    refused_detail.store(why | unsigned{state} << 8, std::memory_order_relaxed);
    refused_address.store(address, std::memory_order_release);
}
} // namespace

// The last write the service refused (headless/crash_report.cpp): its address, 0 for none.
extern "C" std::uintptr_t eden_guest_write_refused(unsigned* why, unsigned* state) noexcept {
    const std::uintptr_t address = refused_address.load(std::memory_order_acquire);
    const unsigned detail = refused_detail.load(std::memory_order_relaxed);
    *why = detail & 0xff;
    *state = detail >> 8;
    return address;
}

#ifdef PS5_NATIVE
extern "C" const char __eden_text_start[] __attribute__((weak));
extern "C" const char __eden_text_end[] __attribute__((weak));
#endif

namespace {
// Who writes the slots that get promoted. A fault handler (ServiceSignal) reads the interrupted
// instruction's address and the first return address of the app's code above the stack pointer:
// a write by the emulated CPU has the JIT's entry point there, a write by the emulator itself the
// function that called the copy. The promotions are counted by that place (ServiceReport).
constexpr std::size_t WriterPcs = 4;
struct WriterSite {
    std::atomic<std::uintptr_t> key{0};
    std::uintptr_t pcs[WriterPcs]{};
    std::atomic<std::uint64_t> count{0};
};
constexpr std::size_t WriterSites = 512;
WriterSite writer_sites[WriterSites];
std::atomic<std::uint64_t> promotions_in_app_code{0}, promotions_elsewhere{0};
thread_local std::uintptr_t fault_pcs[WriterPcs] = {};
thread_local bool fault_in_app_code = false;

void NoteWriter() noexcept {
    (fault_in_app_code ? promotions_in_app_code : promotions_elsewhere).fetch_add(1, std::memory_order_relaxed);
    std::uintptr_t key = 0xCBF29CE484222325ull;
    for (const std::uintptr_t pc : fault_pcs) key = (key ^ pc) * 0x100000001B3ull;
    key |= 1;
    std::size_t at = static_cast<std::size_t>((key >> 12) % WriterSites);
    for (std::size_t probe = 0; probe < 16; ++probe, at = (at + 1) % WriterSites) {
        std::uintptr_t expected = 0;
        if (writer_sites[at].key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
            for (std::size_t i = 0; i < WriterPcs; ++i) writer_sites[at].pcs[i] = fault_pcs[i];
            writer_sites[at].count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (expected == key) {
            writer_sites[at].count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}
} // namespace

namespace {
// Writers waiting for a block (WaitForBlock), and how many waits ended with one.
std::atomic<unsigned> memory_waiters{0};
std::atomic<std::uint64_t> memory_waits_won{0}, memory_waits_lost{0};

// No block was left for a written slot. Most of what the pool gives out is held by the graphics
// caches, which free what is not drawn once they see memory short (headless/performance.cpp reads
// eden_guest_memory_wanted, and while a writer waits the caches see no free memory at all). The
// game's own memory comes first: a large open-world game wrote 1.8 GB of new memory within 10 s
// after 23 minutes of play, faster than the caches free at the end of each frame, and the write
// that found no block ended the session. So the writer waits here, in its fault handler, and tries
// again every 20 ms for up to 2 s while the caches give memory back. Only then is it refused.
std::size_t RecoverMemory(std::size_t bytes, bool wait, bool driver, int keeper) noexcept;
template <typename Promote>
bool WaitForBlock(Promote&& promote) noexcept {
    memory_waiters.fetch_add(1, std::memory_order_acq_rel);
    bool ok = false;
    for (unsigned tries = 0; tries < 100 && !ok; ++tries) {
        // Slots nothing touches give their blocks up first (packed, or to the swap store): without
        // waiting for a pass that is packing, and without the driver (this is a fault handler).
        if (RecoverMemory(2 * SparseSlot, false, false, 0) == 0) {
            const timespec pause{0, 20'000'000};
            nanosleep(&pause, nullptr);
        }
        ok = promote();
    }
    memory_waiters.fetch_sub(1, std::memory_order_acq_rel);
    (ok ? memory_waits_won : memory_waits_lost).fetch_add(1, std::memory_order_relaxed);
    Note(ok ? "EDEN_CRAM a written slot waited for the graphics caches to give memory back, and got a block\n"
            : "EDEN_CRAM a written slot waited 2 s for memory and found no block\n");
    return ok;
}
} // namespace

// How many writes of the guest's memory are waiting for a block now (headless/performance.cpp):
// while any is, the graphics caches are told no memory is free.
// A page of the guest's memory the GPU's caches now hold (+1) or no longer hold (-1), from the core's
// device memory manager. Counted per slot; packing skips slots with any (PackSlot).
extern "C" void eden_guest_gpu_cached(const void* host, int delta) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range == nullptr) return;
    const std::uintptr_t begin = range->begin.load(std::memory_order_relaxed);
    const auto at = reinterpret_cast<std::uintptr_t>(host);
    if (at < begin || at >= range->end) return;
    auto* table = gpu_cached_table.load(std::memory_order_acquire);
    if (table == nullptr) {
        const std::size_t slots = (range->end - begin) / SparseSlot;
        auto* made = new (std::nothrow) std::atomic<std::int32_t>[slots]();
        if (made == nullptr) return;
        gpu_cached_slots = slots;
        if (gpu_cached_table.compare_exchange_strong(table, made, std::memory_order_acq_rel)) table = made;
        else delete[] made;
    }
    const std::size_t slot = (at - begin) / SparseSlot;
    if (slot < gpu_cached_slots) table[slot].fetch_add(delta, std::memory_order_acq_rel);
}

extern "C" unsigned eden_guest_memory_wanted(void) noexcept {
    return memory_waiters.load(std::memory_order_acquire);
}

// Last resort for a host heap piece, after the direct pool refused even 2 MiB.
// Transfer ownership under the reserve lock: never release then race the keeper
// to reacquire the same memory. The host's committed-block table owns/frees it.
// Leave at least half the configured reserve for guest write faults, and give
// waiting guest writers priority. This does not change the guest's own fallback.
bool TakeHostReserveBlock(std::int64_t* physical) noexcept {
#ifdef PS5_NATIVE
    const char* enabled = std::getenv("EDEN_HOST_RESERVE_TRANSFER");
    if (!physical || !enabled || std::strcmp(enabled, "1") != 0) return false;
    Spin(reserve_lock);
    const auto count = reserve_count.load(std::memory_order_relaxed);
    const bool allowed = service_range.load(std::memory_order_acquire) != nullptr &&
        memory_waiters.load(std::memory_order_acquire) == 0 && count > ReserveTarget() / 2;
    if (allowed) {
        *physical = reserve[count - 1];
        reserve_count.store(count - 1, std::memory_order_relaxed);
    }
    reserve_lock.clear(std::memory_order_release);
    if (allowed) counters.host_reserve_used.fetch_add(1, std::memory_order_relaxed);
    return allowed;
#else
    (void)physical;
    return false;
#endif
}

// The guest's headroom (the reserve): what it holds and what it is to hold, in bytes; both 0 while
// the service does not run or keeps no reserve (the host). headless/performance.cpp counts what is
// not held yet as taken, as all of it was before.
extern "C" void eden_guest_memory_reserve(std::size_t* held, std::size_t* target) noexcept {
#ifdef PS5_NATIVE
    const bool running = service_range.load(std::memory_order_acquire) != nullptr;
    *held = running ? reserve_count.load(std::memory_order_relaxed) * SparseSlot : 0;
    *target = running ? ReserveTarget() * SparseSlot : 0;
#else
    *held = 0;
    *target = 0;
#endif
}

// How the waits ended so far, and the writes refused for want of a block (headless/performance.cpp
// ReportMemory prints them beside what holds the pool).
extern "C" void eden_guest_memory_waits(unsigned long long* won, unsigned long long* lost,
                                        unsigned long long* refused) noexcept {
    *won = memory_waits_won.load(std::memory_order_relaxed);
    *lost = memory_waits_lost.load(std::memory_order_relaxed);
    *refused = counters.failures.load(std::memory_order_relaxed);
}

// A fault at `address`: true when it was an access to a slot of the guest's memory that can now
// succeed (the faulting instruction repeats), false for anything else. `write`: 1 a write, 0 a
// read, -1 not known (taken as a write).
bool HandleGuestWriteFault(void* fault, int write) noexcept {
    auto address = reinterpret_cast<std::uintptr_t>(fault);
    if (const auto expected = probe_expected.load(std::memory_order_acquire); expected != 0 && address != expected) {
        probe_mismatch.store(true, std::memory_order_relaxed);
        address = expected;
    }
    SparseRange* const range = WriteRangeOf(address);
    if (range == nullptr) return false;
    const std::size_t slot = (address - range->begin.load(std::memory_order_relaxed)) / SparseSlot;
    // The same address faulting again and again with nothing to change is not a slot to promote
    // but a real fault. A promotion starts the count again (below).
    static thread_local std::uintptr_t last = 0;
    static thread_local unsigned repeats = 0;
    repeats = address == last ? repeats + 1 : 1;
    last = address;
    if (repeats > 16) {
        repeats = 0;
        Refused(address, 1, SlotState(*range, slot));
        Note("EDEN_CRAM refused a write: 17 faults in a row at one address, its slot unchanged\n");
        return false;
    }
    counters.faults.fetch_add(1, std::memory_order_relaxed);
    const bool waited = LockSlot(*range, slot, true);
    const std::uint8_t state = SlotState(*range, slot);
    bool in_units = false;
#ifdef PS5_NATIVE
    if (const PackedSlot* const packed = packed_slots.load(std::memory_order_acquire); state == SlotPacked && packed != nullptr)
        in_units = packed[slot].units;
#endif
    if (waited && !in_units) {
        // Someone changed the slot meanwhile (or is mapping it again): the access may now succeed,
        // and a read must never make a Fine slot whole. It faults again if it was a write.
        // (Not so for a slot in units: who held it brought back a unit, maybe another one. This
        // unit is looked at now, under the lock; returning to fault again could lose the lock to
        // other units' threads 16 times running and be refused.)
        UnlockSlot(*range, slot, state);
        return true;
    }
    std::uint8_t to = SlotOwn;
    bool kept_clean = false;
    const auto promote = [&]() noexcept {
        bool done = true;
        if (state == SlotFine) {
            done = PromoteFine(*range, slot);
        } else if (state == SlotZero) {
            done = PromoteZero(*range, slot);
            if (done) {
                counters.promotions.fetch_add(1, std::memory_order_relaxed);
                NoteWriter();
            }
        } else if (state == SlotShared) {
            done = PromoteShared(*range, slot);
        } else if (state == SlotPacked) {
#ifdef PS5_NATIVE
            // In units: the unit touched comes back, the slot stays Packed (a read that faults
            // again at once is taken for a write, as for a Clean slot below).
            if (in_units)
                done = UnitBack(*range, slot, address, write != 0 || repeats > 1, &to, &kept_clean);
            else
#endif
            done = UnpackSlot(*range, slot, &to);
            if (done) CountUnpacker(slot);
        } else if (state == SlotIdle) {
            to = WakeIdle(*range, slot); // any access: Own again, or Clean (a write there faults once more)
        } else if (state == SlotClean) {
            // Only a write faults on a Clean slot. A read that got here faulted while the slot was
            // Idle, and another thread woke it first: it can be read as it is, and keeps its copy
            // in the swap store (dropped for a read, the slot would also count as written again).
            // Should the fault's write bit ever be wrong, the access faults here again at once,
            // and is then taken for the write it is.
            if (write == 0 && repeats > 1) clean_refaults.fetch_add(1, std::memory_order_relaxed);
            if (write != 0 || repeats > 1) done = MakeDirty(*range, slot);
            else to = SlotClean, kept_clean = true, clean_late_reads.fetch_add(1, std::memory_order_relaxed);
        }
        return done;
    };
    bool ok = promote();
    if (!ok) ok = WaitForBlock(promote);
    UnlockSlot(*range, slot, ok ? to : state);
    if (!ok) {
        counters.failures.fetch_add(1, std::memory_order_relaxed);
        Refused(address, 2, state);
        Note("EDEN_CRAM out of memory: a written slot of the guest's memory found no block\n");
    } else if (state != SlotOwn && !kept_clean) {
        // The slot was made writable. A game that writes one place each time its slot was given
        // back (cleared, freed or scanned) faults there again and again over a session, each time
        // with something to do: counted as repeats, the 17th such write ended the game after
        // 15 to 60 minutes (Swordpdf/ProsperoEden#246).
        repeats = 0;
    }
    return ok;
}

namespace {
struct sigaction service_previous_segv{}, service_previous_bus{};

void ServiceSignal(int signal, siginfo_t* info, void* context) {
#ifdef PS5_NATIVE
    fault_in_app_code = false;
    for (std::uintptr_t& pc : fault_pcs) pc = 0;
    if (context != nullptr) {
        const auto low = reinterpret_cast<std::uintptr_t>(__eden_text_start) + 8;
        const auto high = reinterpret_cast<std::uintptr_t>(__eden_text_end);
        const std::uintptr_t rip = Eden::Fastmem::ContextRip(context);
        fault_in_app_code = rip >= low && rip < high;
        const auto* stack = reinterpret_cast<const std::uintptr_t*>(Eden::Fastmem::ContextRsp(context));
        std::size_t found = 0;
        // Never past the page (16 KiB) that holds the stack pointer: a thread's stack can end a few
        // hundred bytes above it, and what lies after it is not mapped.
        const auto page_end = ((reinterpret_cast<std::uintptr_t>(stack) | 0x3FFFu) + 1);
        const auto room = static_cast<unsigned>((page_end - reinterpret_cast<std::uintptr_t>(stack)) / sizeof(std::uintptr_t));
        for (unsigned i = 0; i < 160 && i < room && found < WriterPcs; ++i) {
            if (stack[i] >= low && stack[i] < high && (found == 0 || fault_pcs[found - 1] != stack[i])) {
                fault_pcs[found++] = stack[i];
            }
        }
    }
#endif
    if (HandleGuestWriteFault(info ? info->si_addr : nullptr, context ? Eden::Fastmem::ContextFaultWrite(context) : -1)) return;
    const struct sigaction& previous = signal == SIGSEGV ? service_previous_segv : service_previous_bus;
    if (previous.sa_flags & SA_SIGINFO) {
        previous.sa_sigaction(signal, info, context);
    } else if (previous.sa_handler == SIG_DFL) {
        std::signal(signal, SIG_DFL); // the faulting instruction repeats and ends the process
    } else if (previous.sa_handler != SIG_IGN) {
        previous.sa_handler(signal);
    }
}

bool InstallServiceSignal() noexcept {
    static std::once_flag once;
    static bool installed = false;
    std::call_once(once, [] {
        struct sigaction action{};
        action.sa_sigaction = ServiceSignal;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        installed = sigaction(SIGSEGV, &action, &service_previous_segv) == 0 &&
                    sigaction(SIGBUS, &action, &service_previous_bus) == 0;
    });
    return installed;
}

// Gives a range slot states (a guest range, or the self-test's).
bool AddStates(SparseRange& range) noexcept {
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    auto* state = new (std::nothrow) std::atomic<std::uint8_t>[slots];
    auto* fine = state ? new (std::nothrow) std::uint16_t[slots]() : nullptr;
    auto* fine_after = fine ? new (std::nothrow) std::uint32_t[slots]() : nullptr;
    auto* aliases = fine_after && range.guest ? new (std::nothrow) std::uintptr_t[slots * AliasesPerSlot]() : nullptr;
    if (fine_after == nullptr || (range.guest && aliases == nullptr)) {
        delete[] state;
        delete[] fine;
        delete[] fine_after;
        return false;
    }
    range.fine = fine;
    range.fine_after = fine_after;
    range.aliases = aliases;
    for (std::size_t slot = 0; slot < slots; ++slot)
        state[slot].store(range.owned[slot].load(std::memory_order_relaxed) ? SlotOwn : SlotZero, std::memory_order_relaxed);
    range.state = state;
    return true;
}

// ---- The scanner ----
std::thread scanner;
std::mutex scanner_mutex;
std::condition_variable scanner_wake;
bool scanner_stop = false;
#ifdef PS5_NATIVE
// The reserve kept full: every 100 ms, so a burst of writes that used it finds it whole again as
// soon as the caches have given memory back (the scanner only passes every 20 s).
std::thread keeper;

void KeepFree() noexcept; // below: slots nothing touches give their blocks up while the pool is nearly empty
void ReserveKeeper() {
    std::unique_lock lock{scanner_mutex};
    for (unsigned tick = 1; !scanner_stop; ++tick) {
        scanner_wake.wait_for(lock, std::chrono::milliseconds(100), [] { return scanner_stop; });
        if (scanner_stop) break;
        lock.unlock();
        FillReserve();
        if (tick % 2 == 0) KeepFree();
        lock.lock();
    }
}
#endif

std::uint64_t Mix(std::uint64_t hash, std::uint64_t word) noexcept {
    hash ^= word + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash * 0xff51afd7ed558ccdull;
}
std::uint64_t SampleHash(std::uintptr_t at) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    std::uint64_t hash = 0;
    for (std::size_t i = 0; i < SparseSlot / 8; i += SparseSlot / 8 / 64) hash = Mix(hash, word[i] ^ word[i + 1]);
    return hash;
}
std::uint64_t FullHash(std::uintptr_t at, std::size_t bytes) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    std::uint64_t hash = 0;
    for (std::size_t i = 0; i < bytes / 8; ++i) hash = Mix(hash, word[i]);
    return hash;
}

void Fence(std::uintptr_t at) noexcept {
    // A window alias would write past the fence.
    if (SparseRange* range = guest_range.load(std::memory_order_acquire); range && range->aliases &&
        at >= range->begin.load(std::memory_order_relaxed) && at < range->end)
        DropSlotAliases(*range, (at - range->begin.load(std::memory_order_relaxed)) / SparseSlot);
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ) != 0) std::abort();
}
void Unfence(std::uintptr_t at) noexcept {
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ | PROT_WRITE) != 0) std::abort();
}

// An Own slot that holds only zeroes goes back. Fenced while it is looked at again.
bool ScanZero(SparseRange& range, std::size_t slot) noexcept {
    LockSlot(range, slot);
    if (SlotState(range, slot) != SlotOwn) {
        UnlockSlot(range, slot, SlotState(range, slot));
        return false;
    }
    const std::uintptr_t at = SlotAddress(range, slot);
    Fence(at);
    const bool zero = AllZero(at);
    if (zero) ReturnSlot(range, slot);
    else Unfence(at);
    UnlockSlot(range, slot, zero ? SlotZero : SlotOwn);
    return zero;
}

// An Own slot with many pages of zeroes becomes Fine. A racy count first, then the count again fenced.
bool ScanFine(SparseRange& range, std::size_t slot) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
    if (counters.passes.load(std::memory_order_relaxed) < range.fine_after[slot]) return false;
    std::size_t zeros = 0;
    for (std::size_t page = 0; page < FinePages; ++page) zeros += PageZero(at + page * FinePage);
    if (zeros < FineMinZeroPages) return false;
    LockSlot(range, slot);
    if (SlotState(range, slot) != SlotOwn) {
        UnlockSlot(range, slot, SlotState(range, slot));
        return false;
    }
    Fence(at);
    bool zero[FinePages];
    zeros = 0;
    for (std::size_t page = 0; page < FinePages; ++page) zeros += zero[page] = PageZero(at + page * FinePage);
    bool fined = false;
    if (zeros == FinePages) {
        ReturnSlot(range, slot);
        UnlockSlot(range, slot, SlotZero);
        counters.returns_scan.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (zeros >= FineMinZeroPages) {
        fined = MakeFine(range, slot, zero, FinePages - zeros);
        if (!fined) counters.fine_failures.fetch_add(1, std::memory_order_relaxed);
    }
    if (!fined) Unfence(at);
    UnlockSlot(range, slot, fined ? SlotFine : SlotOwn);
    return fined;
}

// Two slots of the same bytes show one block (the console; the host only counts them).
bool ScanShare(SparseRange& range, std::size_t keep, std::size_t other) noexcept {
    const std::size_t first = std::min(keep, other), second = std::max(keep, other);
    LockSlot(range, first);
    LockSlot(range, second);
    const std::uint8_t keep_state = SlotState(range, keep), other_state = SlotState(range, other);
    bool shared = false;
    if ((keep_state == SlotOwn || keep_state == SlotShared) && other_state == SlotOwn) {
        const std::uintptr_t keep_at = SlotAddress(range, keep), other_at = SlotAddress(range, other);
        if (keep_state == SlotOwn) Fence(keep_at);
        Fence(other_at);
        const bool same = std::memcmp(reinterpret_cast<const void*>(keep_at), reinterpret_cast<const void*>(other_at),
                                      SparseSlot) == 0;
#ifdef PS5_NATIVE
        // A block becoming shared counts both slots (the first ShareBlock makes it 1, the second 2); one
        // already shared counts one more. A table that is full shares nothing.
        bool counted = false;
        if (same && ShareBlock(range.physical[keep])) {
            counted = keep_state == SlotShared || ShareBlock(range.physical[keep]);
            if (!counted) (void)UnshareBlock(range.physical[keep]);
        }
        if (counted) {
            if (!MapSlot(other_at, PROT_READ, range.physical[keep])) std::abort();
            if (sceKernelReleaseDirectMemory(range.physical[other], SparseSlot) != 0) std::abort();
            range.physical[other] = range.physical[keep];
            guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
            shared = true;
        }
#else
        (void)same;
#endif
        if (!shared) {
            if (keep_state == SlotOwn) Unfence(keep_at);
            Unfence(other_at);
        }
        if (same) counters.dedup.fetch_add(1, std::memory_order_relaxed);
    }
    UnlockSlot(range, second, second == keep ? (shared ? SlotShared : keep_state) : (shared ? SlotShared : other_state));
    UnlockSlot(range, first, first == keep ? (shared ? SlotShared : keep_state) : (shared ? SlotShared : other_state));
    return shared;
}

// ---- Packing (see "Packed slots" above) ----
// EDEN_CRAM_PACK=0 turns it off. While the pool's largest free block is under PackBelowLargest,
// each scan pass packs up to PackPerPass slots unwritten for ColdSeconds or more, the
// coldest first. A slot packed before waits twice as many passes each time (it came back), and
// one that does not compress to PackMaxRatio is left as it is.
// On unless EDEN_CRAM_PACK=0 (2026-11-09). It only starts once the pool's largest free block is
// below EDEN_CRAM_PACK_BELOW_MIB, so a game that fits never packs. Without PS5 Pro memory (12,224
// MiB) Mario Kart World ran out at every race load without it; with it (GPU-reachable slots
// included, below) it ran a whole race and into the next (docs/MKW-MEMORY-LEAK.md). It was off from
// 2026-11-08: sessions that packed showed impossible draws and GPU hangs, but those draws turned out
// to come without packing too (the race-load event), and the hangs with GPU threads waiting for
// memory went with the unpack reserve (EDEN_CRAM_UNPACK_KEEP_MIB).
bool PackEnabled() noexcept {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_CRAM_PACK");
        return (value == nullptr || value[0] != '0') && &LZ4_compress_default != nullptr &&
               &LZ4_decompress_safe != nullptr && &LZ4_compressBound != nullptr;
    }();
    return on;
}
constexpr std::size_t PackPerPass = 256;                       // 512 MiB of slots
// EDEN_CRAM_PACK_BELOW_MIB / EDEN_CRAM_PACK_COLD_S: when packing starts (largest free block) and how long
// a slot must stay unwritten (tuning; defaults 3072 MiB and 30 s: MKW without Pro memory needs packing
// to start well before the race load, which takes ~1.5 GB at once).
std::size_t EnvOr(const char* name, std::size_t fallback) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && *value ? static_cast<std::size_t>(std::strtoull(value, nullptr, 10)) : fallback;
}
std::size_t PackBelowLargest() noexcept {
    static const std::size_t bytes = EnvOr("EDEN_CRAM_PACK_BELOW_MIB", 3072) << 20;
    return bytes;
}
constexpr double PackMaxRatio = 0.6;
std::vector<std::uint8_t> pack_times; // per slot, the scanner's: how often it was packed
std::atomic<bool> pack_refused{false};
std::mutex pack_mutex; // the scanner's cold pass and packing, and eden_memory_recover
thread_local bool holding_pack = false; // this thread holds pack_mutex (a heap growth inside must not wait for it)
std::vector<std::size_t> recover_order; // eden_memory_recover's list, room made once (no allocation there)
std::vector<std::size_t> pack_ready;    // PackPass's, likewise
std::vector<char> pack_scratch;

#ifdef PS5_NATIVE
extern "C" [[gnu::weak]] std::int32_t sceKernelAvailableDirectMemorySize(std::int64_t, std::int64_t, std::size_t, std::int64_t*, std::size_t*);
#endif
bool PoolShortForPacking() noexcept {
#ifdef PS5_NATIVE
    const auto total = sceKernelGetDirectMemorySize();
    std::int64_t start = 0;
    std::size_t largest = 0;
    return total > 0 && &sceKernelAvailableDirectMemorySize != nullptr &&
           sceKernelAvailableDirectMemorySize(0, total, SparseSlot, &start, &largest) == 0 &&
           largest < PackBelowLargest();
#else
    return false;
#endif
}

// What the pool has free in aligned 2 MiB pieces (what a guest slot, the driver and the heap take).
// 0 when it cannot be read.
#ifdef PS5_NATIVE
struct PoolRegion { std::int64_t start; std::int64_t end; std::int32_t type; };
extern "C" [[gnu::weak]] std::int32_t sceKernelDirectMemoryQuery(std::int64_t, int, PoolRegion*, std::size_t);
#endif
std::size_t PoolFreePieces() noexcept {
#ifdef PS5_NATIVE
    const std::int64_t total = sceKernelGetDirectMemorySize();
    if (total <= 0 || &sceKernelDirectMemoryQuery == nullptr) return 0;
    const auto pieces = [](std::int64_t from, std::int64_t to) noexcept {
        const std::int64_t first = (from + std::int64_t(SparseSlot) - 1) / std::int64_t(SparseSlot);
        const std::int64_t last = to / std::int64_t(SparseSlot);
        return last > first ? static_cast<std::size_t>(last - first) * SparseSlot : std::size_t{0};
    };
    std::size_t free_bytes = 0;
    std::int64_t offset = 0;
    unsigned regions = 0;
    PoolRegion region{};
    while (offset < total && regions < 16384 && sceKernelDirectMemoryQuery(offset, 1, &region, sizeof(region)) == 0 &&
           region.end > offset) {
        free_bytes += pieces(offset, region.start);
        offset = region.end;
        ++regions;
    }
    if (regions < 16384) free_bytes += pieces(offset, total);
    return free_bytes;
#else
    return 0;
#endif
}
// Idle slots (above): how long a slot must stay untouched before it is packed, how long a touched
// slot is left alone (doubled each time it was touched, up to 8 times: ProbeSlot), and how many
// slots a pass may make Idle (2 GiB).
std::size_t IdleSeconds() noexcept {
    static const std::size_t seconds = EnvOr("EDEN_CRAM_IDLE_S", 10);
    return seconds;
}
std::size_t IdleBackoff() noexcept {
    static const std::size_t seconds = EnvOr("EDEN_CRAM_IDLE_BACKOFF_S", 5);
    return seconds;
}
constexpr std::size_t IdlePerPass = 1024;

// (ColdPass, below: a sampled hash of each Own slot, and when it last changed.)
std::vector<std::uint64_t> cold_hash;
std::vector<std::uint32_t> cold_since; // per Own slot: when its hash last changed (s, steady clock)

// Slots that did not compress when last looked at, until they are written again (ColdPass sees their
// sampled hash change): LZ4 is not tried on them again.
std::uint8_t* pack_raw = nullptr;
// Slots the swap store did not hold all of when they were last to go there without a write, until
// they are written again: not hashed and looked up again for nothing.
std::uint8_t* swap_absent = nullptr;
// A recovery is packing (pack_mutex held): a packed slot's block goes straight back to the pool, not
// to the blocks kept for unpacking, and pack_released tells whether it did (the store may have taken
// it instead). The driver asked 32 MiB back, was told 32 MiB, and found none: the blocks had gone to
// refill what unpacks had used, and its allocation ended the session.
bool recovering = false, pack_released = false;
std::atomic<std::uint64_t> swap_last_resort{0}; // slots that went to the swap store as a request's last resort (RecoverMemory)
// May the slot go to the swap store: never; only if that writes nothing (the store holds all of it);
// with writes, inside the budget's part that is not kept for failed allocations; with writes down
// to the budget's last byte.
enum class Swap { No, Reuse, Yes, Urgent };

#ifdef PS5_NATIVE
// ---- Units: how a slot goes in units, and what becomes of its units afterwards (see "Units" above) ----
std::size_t ProbeColdSeconds() noexcept;
std::uint8_t* unit_scratch = nullptr; // a slot's packed units, side by side (pack_mutex)
std::uint8_t* unit_stage = nullptr;   // idle units' bytes at their places in a slot (pack_mutex)
bool UnitsReady() noexcept {
    return unit_zstd_ready.load(std::memory_order_acquire) && unit_slots != nullptr && unit_bytes != nullptr &&
           unit_poor != nullptr && swap_units != nullptr && unit_scratch != nullptr && unit_stage != nullptr &&
           slab_base != 0 && IdleEnabled() && idle_since != nullptr && !PackKeep();
}

// Idle -> Packed, in units (the slot is locked and Idle; pack_mutex held).
bool PackSlotUnits(SparseRange& range, std::size_t slot, Swap swap) noexcept {
    PackedSlot* const table = packed_slots.load(std::memory_order_acquire);
    const std::uintptr_t at = SlotAddress(range, slot);
    const SwapWrites writes = swap == Swap::Urgent ? SwapWrites::Urgent : swap == Swap::Yes ? SwapWrites::Budget : SwapWrites::None;
    if (writes == SwapWrites::None && unit_poor[slot] != 0) {
        UnlockSlot(range, slot, SlotIdle);
        pack_skips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Readable, not writable, while its units are looked at.
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ) != 0) std::abort();
    std::uint32_t taken[SwapUnits] = {};
    std::uint32_t got = 0;
    const auto leave = [&]() noexcept {
        for (std::size_t unit = 0; unit < SwapUnits; ++unit)
            if (got >> unit & 1) (void)UnitGive(taken[unit]);
        if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_NONE) != 0) std::abort();
        UnlockSlot(range, slot, SlotIdle);
        pack_skips.fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    std::uint32_t units[SwapUnits];
    std::uint16_t sizes[SwapUnits];
    bool wrote = false;
    const std::size_t used = UnitsOut(reinterpret_cast<const std::uint8_t*>(at), 0xFFFFFFFFu, units, sizes, unit_scratch, 0, writes, &wrote);
    std::uint32_t stay = 0;
    std::size_t packed_units = 0;
    for (std::size_t unit = 0; unit < SwapUnits; ++unit) {
        if (units[unit] == 0) stay |= 1u << unit;
        else if (units[unit] != UnitZero && (units[unit] & UnitPacked)) ++packed_units;
    }
    const std::size_t staying = static_cast<std::size_t>(__builtin_popcount(stay));
    if (stay != 0 && SparseSlot - staying * SwapUnit - used < UnitMinGain) {
        if (writes == SwapWrites::None) unit_poor[slot] = 1;
        return leave();
    }
    // Memory of their own for the units that stay.
    const std::size_t slabs_before = slab_held.load(std::memory_order_relaxed);
    for (std::size_t unit = 0; unit < SwapUnits; ++unit) {
        if (!(stay >> unit & 1)) continue;
        if (!UnitTake(&taken[unit])) return leave();
        got |= 1u << unit;
        std::memcpy(UnitView(taken[unit]), reinterpret_cast<const void*>(at + unit * SwapUnit), SwapUnit);
    }
    if (used != 0 && !StoreStart()) return leave();
    DropSlotAliases(range, slot);
    // The slot's address space is a reservation from here: every access faults and waits for the slot.
    const int reserved = ReserveOver(reinterpret_cast<void*>(at), SparseSlot, SparseSlot);
    if (reserved == -1) return leave();
    if (reserved != 1) std::abort(); // unmapped and not reserved: another mapping could land in the guest's range
    std::int64_t donate = range.physical[slot];
    void* data = nullptr;
    if (used != 0) {
        data = StoreAlloc(used, &donate);
        if (data == nullptr) {
            // No room in the packed store: the block, untouched, comes back.
            if (!MapSlot(at, PROT_NONE, range.physical[slot])) std::abort();
            for (std::size_t unit = 0; unit < SwapUnits; ++unit)
                if (got >> unit & 1) (void)UnitGive(taken[unit]);
            UnlockSlot(range, slot, SlotIdle);
            pack_skips.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::memcpy(data, unit_scratch, used);
    }
    pack_released = false;
    if (donate >= 0 && (recovering || !KeepForUnpack(donate))) {
        if (sceKernelReleaseDirectMemory(donate, SparseSlot) != 0) std::abort();
        // (Not counted as given back when the units that stay took a new slab.)
        pack_released = slab_held.load(std::memory_order_relaxed) <= slabs_before;
    }
    range.physical[slot] = -1;
    for (std::size_t unit = 0; unit < SwapUnits; ++unit) {
        const bool stays = (stay >> unit & 1) != 0;
        swap_units[slot * SwapUnits + unit] = stays ? UnitHere | taken[unit] : units[unit];
        unit_bytes[slot * SwapUnits + unit] = stays ? std::uint16_t{0} : sizes[unit];
        if (stays) slab_owner[taken[unit]] = static_cast<std::uint32_t>(slot * SwapUnits + unit + 1);
    }
    unit_slots[slot] = {stay, stay, idle_since[slot], 0};
    table[slot] = {data, static_cast<std::uint32_t>(used), 0, 0, true};
    guest_committed.fetch_sub(SparseSlot - staying * SwapUnit, std::memory_order_relaxed);
    packed_count.fetch_add(1, std::memory_order_relaxed);
    packed_bytes.fetch_add(used, std::memory_order_relaxed);
    packs.fetch_add(1, std::memory_order_relaxed);
    unit_slots_made.fetch_add(1, std::memory_order_relaxed);
    unit_packed.fetch_add(packed_units, std::memory_order_relaxed);
    unit_stayed.fetch_add(staying, std::memory_order_relaxed);
    if (wrote && slot < cold_since.size()) NoteSwapWrite(slot, ColdNow() - cold_since[slot], true);
    UnlockSlot(range, slot, SlotPacked);
    return true;
}

// The idle units of a slot in units go the way its others went (the slot is locked; pack_mutex
// held). Returns how many went; *blocks: slabs that went back to the pool with them.
std::size_t UnitsEvict(std::size_t slot, SwapWrites writes, std::size_t* blocks) noexcept {
    PackedSlot* const table = packed_slots.load(std::memory_order_acquire);
    UnitSlot& state = unit_slots[slot];
    const std::uint32_t mask = state.idle & state.here;
    if (mask == 0) return 0;
    std::uint32_t* const records = swap_units + slot * SwapUnits;
    std::uint16_t* const bytes = unit_bytes + slot * SwapUnits;
    // The slot's packed piece is made anew: the units it still holds first, then those that join them.
    std::uint32_t moved[SwapUnits];
    std::size_t used = 0;
    const auto* const old = static_cast<const std::uint8_t*>(table[slot].data);
    for (std::size_t unit = 0; unit < SwapUnits; ++unit) {
        moved[unit] = records[unit];
        if ((state.here >> unit & 1) || records[unit] == UnitZero || !(records[unit] & UnitPacked)) continue;
        if (old == nullptr) return 0; // (never: a packed unit has its piece)
        std::memcpy(unit_scratch + used, old + (records[unit] & UnitIndex), bytes[unit]);
        moved[unit] = UnitPacked | static_cast<std::uint32_t>(used);
        used += bytes[unit];
    }
    for (std::size_t unit = 0; unit < SwapUnits; ++unit)
        if (mask >> unit & 1) std::memcpy(unit_stage + unit * SwapUnit, UnitView(records[unit] & UnitIndex), SwapUnit);
    std::uint32_t units[SwapUnits];
    std::uint16_t sizes[SwapUnits];
    bool wrote = false;
    used = UnitsOut(unit_stage, mask, units, sizes, unit_scratch, used, writes, &wrote);
    std::uint32_t going = 0;
    for (std::size_t unit = 0; unit < SwapUnits; ++unit)
        if ((mask >> unit & 1) && units[unit] != 0) going |= 1u << unit;
    if (going == 0) {
        if (writes == SwapWrites::None) unit_poor[slot] = 1;
        return 0;
    }
    void* fresh = nullptr;
    bool anew = true;
    if (used != 0) {
        if (StoreStart()) fresh = StoreAlloc(used, nullptr);
        if (fresh != nullptr) {
            std::memcpy(fresh, unit_scratch, used);
        } else {
            // The pool has no block for the packed store now (this runs when memory is short). The
            // old piece stays as it is, the units that were to join it stay in memory, and those
            // that are zeroes or in the swap store go: what was appended for them is not lost.
            anew = false;
            for (std::size_t unit = 0; unit < SwapUnits; ++unit)
                if ((going >> unit & 1) && units[unit] != UnitZero && (units[unit] & UnitPacked)) going &= ~(1u << unit);
            if (going == 0) return 0;
        }
    }
    if (anew) {
        if (table[slot].data != nullptr) {
            packed_bytes.fetch_sub(table[slot].size, std::memory_order_relaxed);
            StoreRelease(table[slot].data, table[slot].size);
        }
        packed_bytes.fetch_add(used, std::memory_order_relaxed);
        table[slot].data = fresh;
        table[slot].size = static_cast<std::uint32_t>(used);
        state.dead = 0;
    }
    std::size_t gone = 0;
    for (std::size_t unit = 0; unit < SwapUnits; ++unit) {
        if (going >> unit & 1) {
            const std::uint32_t index = records[unit] & UnitIndex;
            records[unit] = units[unit];
            bytes[unit] = sizes[unit];
            if (UnitGive(index) && blocks != nullptr) ++*blocks;
            ++gone;
        } else if (anew && !(state.here >> unit & 1)) {
            records[unit] = moved[unit];
        }
    }
    state.here &= ~going;
    state.idle &= ~going;
    guest_committed.fetch_sub(gone * SwapUnit, std::memory_order_relaxed);
    unit_evicted.fetch_add(gone, std::memory_order_relaxed);
    return gone;
}

// Idle units move out of the slabs that hold few, from the last slab down into room before it, so
// that those slabs go back to the pool (pack_mutex held). A unit that is mapped stays where it is.
void SlabCompact(SparseRange& range) noexcept {
    PackedSlot* const table = packed_slots.load(std::memory_order_acquire);
    if (table == nullptr || slab_base == 0) return;
    std::size_t moves = 0;
    for (std::size_t slab = UnitSlabs; slab-- > 0 && moves < 512;) {
        Spin(slab_lock);
        const std::uint32_t used = slab_physical[slab] >= 0 ? slab_used[slab] : 0;
        slab_lock.clear(std::memory_order_release);
        if (used == 0 || __builtin_popcount(used) > 8) continue;
        for (std::size_t bit = 0; bit < SwapUnits; ++bit) {
            if (!(used >> bit & 1)) continue;
            const std::uint32_t index = static_cast<std::uint32_t>(slab * SwapUnits + bit);
            const std::uint32_t owner = slab_owner[index]; // (looked at again under the slot's lock)
            if (owner == 0) continue;
            const std::size_t slot = (owner - 1) / SwapUnits, unit = (owner - 1) % SwapUnits;
            if (!TryLockSlot(range, slot)) continue;
            const std::uint8_t slot_state = SlotState(range, slot);
            bool room = true;
            if (slot_state == SlotPacked && table[slot].units && (unit_slots[slot].here >> unit & 1) &&
                (unit_slots[slot].idle >> unit & 1) && swap_units[slot * SwapUnits + unit] == (UnitHere | index)) {
                std::uint32_t fresh = 0;
                room = UnitTake(&fresh, slab);
                if (room) {
                    std::memcpy(UnitView(fresh), UnitView(index), SwapUnit);
                    swap_units[slot * SwapUnits + unit] = UnitHere | fresh;
                    slab_owner[fresh] = owner;
                    (void)UnitGive(index);
                    ++moves;
                }
            }
            UnlockSlot(range, slot, slot_state);
            if (!room) {
                unit_moved.fetch_add(moves, std::memory_order_relaxed);
                return; // no slab before this one has room
            }
        }
    }
    unit_moved.fetch_add(moves, std::memory_order_relaxed);
}

// Each slot in units, once a pass or at a request for memory (pack_mutex held): units still idle
// after `idle_for` seconds go; and, for the pass (`cool`), the units of a slot left alone for its
// time lose their access, in one reservation over the slot. Stops once `want` bytes of slabs went
// back (0: no such end). Returns the bytes of slabs that went back to the pool.
std::size_t UnitSweep(SparseRange& range, std::uint32_t now, std::size_t idle_for, bool cool, SwapWrites writes,
                      std::size_t want) noexcept {
    PackedSlot* const table = packed_slots.load(std::memory_order_acquire);
    if (table == nullptr || !UnitsReady()) return 0;
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    const std::size_t before = slab_held.load(std::memory_order_relaxed);
    const std::size_t store_before = store_held.load(std::memory_order_relaxed);
    std::size_t evicted = 0, blocks = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (SlotState(range, slot) != SlotPacked || !table[slot].units) continue;
        const UnitSlot peek = unit_slots[slot]; // (looked at again under the slot's lock)
        const bool due = peek.idle != 0 && now - peek.idle_since >= idle_for &&
                         !(writes == SwapWrites::None && unit_poor[slot] != 0);
        const bool cools = cool && (peek.here & ~peek.idle) != 0 &&
                           now - touch_since[slot].load(std::memory_order_relaxed) >=
                               (IdleBackoff() << std::min<unsigned>(touch_times[slot].load(std::memory_order_relaxed), 3));
        if (!due && !cools) continue;
        if (!TryLockSlot(range, slot)) continue;
        // (Another thread may have made it whole, or given it back, between the look and the lock.)
        const std::uint8_t found = SlotState(range, slot);
        if (found == SlotPacked && table[slot].units) {
            UnitSlot& state = unit_slots[slot];
            if (state.idle != 0 && now - state.idle_since >= idle_for && !(writes == SwapWrites::None && unit_poor[slot] != 0)) {
                evicted += UnitsEvict(slot, writes, &blocks);
            } else if (cool && (state.here & ~state.idle) != 0) {
                // Not while they are written: the sampled hash of the units with access must have
                // stood still for ProbeColdSeconds, as a slot's must (ProbeSlot has why).
                const std::uint32_t awake = state.here & ~state.idle;
                std::uint64_t hash = awake;
                for (std::size_t unit = 0; unit < SwapUnits; ++unit) {
                    if (!(awake >> unit & 1)) continue;
                    const auto* const word =
                        reinterpret_cast<const volatile std::uint64_t*>(UnitView(swap_units[slot * SwapUnits + unit] & UnitIndex));
                    for (std::size_t i = 0; i < SwapUnit / 8; i += SwapUnit / 8 / 16) hash = Mix(hash, word[i] ^ word[i + 1]);
                }
                if (slot >= cold_hash.size() || slot >= cold_since.size()) {
                    // (no place to keep the hash: left as it is)
                } else if (cold_hash[slot] != hash) {
                    cold_hash[slot] = hash;
                    cold_since[slot] = now;
                } else if (now - cold_since[slot] >= ProbeColdSeconds()) {
                    const int reserved = ReserveOver(reinterpret_cast<void*>(SlotAddress(range, slot)), SparseSlot, SparseSlot);
                    if (reserved == 0) std::abort(); // unmapped and not reserved
                    if (reserved == 1) {
                        // (Units idle since before keep their time: they go when theirs is up.)
                        if (state.idle == 0) state.idle_since = now;
                        state.idle = state.here;
                        idled.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
        UnlockSlot(range, slot, found);
        if (swap_refused && writes != SwapWrites::None) break;
        if (want != 0 && blocks * SparseSlot >= want) break;
    }
    if (evicted != 0) SlabCompact(range);
    // Less what the packed store took for the units that went into it.
    const std::size_t after = slab_held.load(std::memory_order_relaxed) + store_held.load(std::memory_order_relaxed);
    return before + store_before > after ? before + store_before - after : 0;
}
#endif

// Idle -> Packed (Own, without Idle slots): the scanner, or a recovery (pack_mutex held). Readable
// and not writable while it is compressed, then the zero block with no access.
bool PackSlot(SparseRange& range, std::size_t slot, Swap swap = Swap::No) noexcept {
#ifdef PS5_NATIVE
    PackedSlot* const table = packed_slots.load(std::memory_order_acquire);
    if (table == nullptr) return false;
    static const bool pack_gpu = [] {
        const char* value = std::getenv("EDEN_CRAM_PACK_GPU");
        return value == nullptr || value[0] != '0';
    }();
    if (auto* gpu_cached = gpu_cached_table.load(std::memory_order_acquire);
        !pack_gpu && gpu_cached != nullptr && slot < gpu_cached_slots &&
        gpu_cached[slot].load(std::memory_order_acquire) > 0) {
        pack_gpu_skips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!pack_gpu && gpu_touched != nullptr && slot < gpu_cached_slots &&
        gpu_touched[slot].load(std::memory_order_acquire) != 0) {
        pack_gpu_skips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!TryLockSlot(range, slot)) return false;
    // Only a slot that is still what it was chosen for: Idle, where slots are made Idle (one touched
    // since its caller listed it is in use again: Own or Clean), else Own.
    const std::uint8_t before = SlotState(range, slot);
    if (before != (IdleEnabled() && idle_since != nullptr ? SlotIdle : SlotOwn)) {
        UnlockSlot(range, slot, before);
        return false;
    }
    const std::uintptr_t at = SlotAddress(range, slot);
    const int access_before = before == SlotOwn ? PROT_READ | PROT_WRITE : PROT_NONE;
    // The slot stays as it was.
    const auto leave = [&](bool skipped) noexcept {
        if (mprotect(reinterpret_cast<void*>(at), SparseSlot, access_before) != 0) std::abort();
        UnlockSlot(range, slot, before);
        if (skipped) pack_skips.fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    const bool in_swap = table[slot].swap != 0; // Idle from Clean: the swap store has its bytes, and where is known
    // In units, when they are on (a slot with a whole copy in the swap store goes as it always did).
    if (!in_swap && before == SlotIdle && UnitsReady()) return PackSlotUnits(range, slot, swap);
    if (in_swap && swap == Swap::No) {
        UnlockSlot(range, slot, before);
        return false;
    }
    int size = 0;
    void* data = nullptr;
    bool to_swap = in_swap;
    std::uint64_t hash = 0;
    std::uint32_t units[SwapUnits];
    if (!in_swap) {
        const bool may_swap = swap != Swap::No && SwapOn() && swap_units != nullptr;
        // Known not to compress: nothing to do unless it may go to the swap store; and one that is to
        // go without a write is not looked up again while it is as it was when the store had not all
        // of it (swap_absent).
        bool compresses = false;
        if (pack_raw != nullptr && pack_raw[slot] != 0) {
            if (!may_swap || (swap == Swap::Reuse && swap_absent != nullptr && swap_absent[slot] != 0)) {
                UnlockSlot(range, slot, before);
                pack_skips.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        // Readable, not writable, while it is compressed (or looked up in the swap store and written there).
        if (before == SlotOwn) Fence(at);
        else if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ) != 0) std::abort();
        if (pack_raw == nullptr || pack_raw[slot] == 0) {
            size = LZ4_compress_default(reinterpret_cast<const char*>(at), pack_scratch.data(),
                                        static_cast<int>(SparseSlot), static_cast<int>(pack_scratch.size()));
            compresses = size > 0 && size <= static_cast<int>(SparseSlot * PackMaxRatio);
            if (pack_raw != nullptr) pack_raw[slot] = compresses ? 0 : 1;
        }
        if (!compresses) {
            // Not for the packed store. To the swap store when it may, while the slot can still be read.
            if (!may_swap) return leave(true);
            const std::uint64_t written = swap_written.load(std::memory_order_relaxed);
            const SwapWrites writes =
                swap == Swap::Reuse ? SwapWrites::None : swap == Swap::Urgent ? SwapWrites::Urgent : SwapWrites::Budget;
            if (!SwapOutSlot(at, units, writes)) {
                if (swap == Swap::Reuse && swap_absent != nullptr) swap_absent[slot] = 1;
                return leave(true);
            }
            hash = FullHash(at, SparseSlot);
            if (slot < cold_since.size())
                NoteSwapWrite(slot, ColdNow() - cold_since[slot], swap_written.load(std::memory_order_relaxed) != written);
            to_swap = true;
        } else if (!StoreStart()) {
            return leave(true);
        }
    }
    DropSlotAliases(range, slot);
    // No access first: from here every reader faults and waits for the slot. Mapped readable even
    // for a moment, the zero block would show zeroes where the guest's data was (a pointer read
    // there made a guest thread jump to address 0).
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_NONE) != 0) std::abort();
    if (!MapSlot(at, PROT_NONE, zero_block)) {
        // The system will not map with no access: the slot stays as it was, and packing stops.
        // (What went to the swap store for it stays there: found again by its contents.)
        pack_refused.store(true, std::memory_order_relaxed);
        Note("EDEN_CRAM_PACK off: the system refused a no-access mapping\n");
        return leave(false);
    }
    // The slot's block, mapped nowhere now: the store may take it when the pool has no block.
    std::int64_t donate = PackKeep() && pack_kept != nullptr ? -1 : range.physical[slot];
    if (!to_swap) {
        data = StoreAlloc(static_cast<std::size_t>(size), &donate);
        if (data == nullptr) {
            // No room anywhere (the store's address space is full): the block, untouched, comes back.
            if (!MapSlot(at, access_before, range.physical[slot])) std::abort();
            UnlockSlot(range, slot, before);
            pack_skips.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::memcpy(data, pack_scratch.data(), static_cast<std::size_t>(size));
    }
    pack_released = false;
    if (PackKeep() && pack_kept != nullptr) {
        pack_kept[slot] = range.physical[slot];
    } else if (donate >= 0 && (recovering || !KeepForUnpack(donate))) {
        if (sceKernelReleaseDirectMemory(donate, SparseSlot) != 0) std::abort();
        pack_released = true;
    }
    range.physical[slot] = -1;
    guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
    if (in_swap) {
        swap_reused.fetch_add(1, std::memory_order_relaxed); // its entry stays: nothing was looked up or written
    } else if (to_swap) {
        std::memcpy(swap_units + slot * SwapUnits, units, sizeof(units));
        table[slot] = {nullptr, 0, 1, hash};
        swap_slots.fetch_add(1, std::memory_order_relaxed);
    } else {
        table[slot] = {data, static_cast<std::uint32_t>(size), 0, 0};
        packed_count.fetch_add(1, std::memory_order_relaxed);
        packed_bytes.fetch_add(static_cast<std::uint64_t>(size), std::memory_order_relaxed);
    }
    packs.fetch_add(1, std::memory_order_relaxed);
    UnlockSlot(range, slot, SlotPacked);
    return true;
#else
    (void)range;
    (void)slot;
    (void)swap;
    return false;
#endif
}

// ---- Research: how much of the held memory is unwritten for a while, and how it compresses ----
// EDEN_CRAM_COLD_REPORT=1: every pass hashes every 64th word of each Own slot; a slot whose hash
// stays the same is counted as unwritten for that many passes (reads are not seen). Up to
// ColdSamples slots unwritten for ColdSeconds are compressed with LZ4 per pass, to estimate
// what compressing them would give back. Measurement only: nothing changes in the slots.
bool ColdReport() noexcept {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_CRAM_COLD_REPORT");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}
constexpr std::size_t ColdSamples = 24;
std::size_t ColdSecondsNow() noexcept {
    static const std::size_t seconds = EnvOr("EDEN_CRAM_PACK_COLD_S", 30);
    return seconds;
}
#define ColdSeconds ColdSecondsNow()
std::uint32_t ColdNow() noexcept {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::size_t cold_rotor = 0;
std::atomic<std::uint64_t> cold_1{0}, cold_3{0}, cold_6{0}, cold_own{0}, cold_zero{0}, cold_zero_old{0};
// Diagnostic (MKW leak): every 64th word of the slot is zero.
bool ColdLooksZero(std::uintptr_t at) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    for (std::size_t i = 0; i < SparseSlot / 8; i += 64)
        if (word[i] != 0) return false;
    return true;
}
std::atomic<std::uint64_t> cold_sampled_bytes{0}, cold_compressed_bytes{0}, cold_compress_ns{0};

std::uint64_t ColdHash(std::uintptr_t at) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    std::uint64_t hash = 0;
    for (std::size_t i = 0; i < SparseSlot / 8; i += 64) hash = Mix(hash, word[i]);
    return hash;
}

void PackPass(SparseRange& range, std::size_t slots, std::vector<std::size_t>& candidates) noexcept;

// A slot is probed (made Idle) when nothing has written it for ProbeColdSeconds (its sampled hash
// stood still that long; a Clean slot cannot have been written) and the time it is left alone after
// a touch has run out. Slots being written are never probed: they would only fault, and a slot
// with no access is one the system cannot read or write on the app's behalf (a file write from a
// guest buffer would fail), which memory in use must not risk. pack_mutex held.
std::size_t ProbeColdSeconds() noexcept {
    static const std::size_t seconds = EnvOr("EDEN_CRAM_PROBE_COLD_S", 8);
    return seconds;
}
bool ProbeSlot(SparseRange& range, std::size_t slot, std::uint32_t now) noexcept {
    const std::uint8_t state = SlotState(range, slot);
    if (state != SlotOwn && state != SlotClean) return false;
    if (state == SlotOwn && now - cold_since[slot] < ProbeColdSeconds()) return false;
    // Left alone after a touch for 5 s, twice as long each time, 40 s at most. It was 320 s: eight
    // minutes into a Mario Kart World session most slots had been touched six times and were looked
    // at once in five minutes; requests that needed 400 MiB found 350 MiB of Idle slots, while
    // 1.5 GB of the game's memory had not been written for a minute. A probe that is touched costs
    // one fault (0.1 ms).
    if (now - touch_since[slot].load(std::memory_order_relaxed) <
        (IdleBackoff() << std::min<unsigned>(touch_times[slot].load(std::memory_order_relaxed), 3)))
        return false;
    if (!TryLockSlot(range, slot)) return false;
    const std::uint8_t locked = SlotState(range, slot);
    const bool made = (locked == SlotOwn || locked == SlotClean) && MakeIdle(range, slot, now);
    UnlockSlot(range, slot, made ? SlotIdle : locked);
    return made;
}
// When memory was refused: how long a slot must have been Idle before it may be taken. Shorter than
// for the scanner's own passes (IdleSeconds): the allocation that gets nothing ends the session.
std::size_t UrgentIdleSeconds() noexcept {
    static const std::size_t seconds = EnvOr("EDEN_CRAM_URGENT_IDLE_S", 2);
    return seconds;
}


// Diagnostic: the zero block (every untouched slot shows it, and the sparse tables' unused parts) must
// stay zero. Written through a mapping that should not allow it, every untouched slot of the game
// would read that garbage. Checked each pass through a read-only view of its own.
std::atomic<std::uint64_t> zero_block_dirty{0};
void CheckZeroBlock() noexcept {
#ifdef PS5_NATIVE
    static const volatile std::uint64_t* view = []() -> const volatile std::uint64_t* {
        void* at = reinterpret_cast<void*>(cpu_mapping_hint);
        if (zero_block < 0 || sceKernelMapDirectMemory(&at, SparseSlot, PROT_READ, 0, zero_block, SparseSlot) != 0)
            return nullptr;
        return static_cast<const volatile std::uint64_t*>(at);
    }();
    if (view == nullptr) return;
    for (std::size_t i = 0; i < SparseSlot / 8; i += 8)
        if (view[i] != 0) {
            if (zero_block_dirty.fetch_add(1, std::memory_order_relaxed) < 4) {
                char line[160];
                const int n = std::snprintf(line, sizeof(line), "EDEN_CRAM_ZERO_BLOCK written: word %zu is %#llx\n", i,
                                            (unsigned long long)view[i]);
                if (n > 0) Report(line);
            }
            return;
        }
#endif
}

#ifdef PS5_NATIVE
extern "C" int pthread_getname_np(pthread_t, char*, std::size_t);
#endif
void CountUnpacker(std::size_t slot) noexcept {
    char name[24] = "?";
#ifdef PS5_NATIVE
    (void)pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
    name[sizeof(name) - 1] = 0;
    if ((std::strcmp(name, "GPU") == 0 || std::strcmp(name, "GPUBack") == 0) && gpu_touched != nullptr &&
        slot < gpu_cached_slots && gpu_touched[slot].exchange(1, std::memory_order_acq_rel) == 0)
        pack_gpu_marked.fetch_add(1, std::memory_order_relaxed);
    while (unpackers_lock.test_and_set(std::memory_order_acquire)) {}
    for (auto& entry : unpackers) {
        if (entry.name[0] == 0) std::memcpy(entry.name, name, sizeof(name));
        if (std::strcmp(entry.name, name) == 0) {
            entry.count.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    unpackers_lock.clear(std::memory_order_release);
}
void ReportUnpackers() noexcept {
    char line[512];
    int length = std::snprintf(line, sizeof(line), "EDEN_CRAM_UNPACKERS");
    for (auto& entry : unpackers) {
        if (entry.name[0] == 0 || length <= 0 || length >= static_cast<int>(sizeof(line)) - 40) break;
        length += std::snprintf(line + length, sizeof(line) - length, " %s=%u", entry.name,
                                entry.count.load(std::memory_order_relaxed));
    }
    if (length > 0 && length < static_cast<int>(sizeof(line)) - 1) {
        line[length] = '\n';
        line[length + 1] = 0;
        Report(line);
    }
}

void ColdPass(SparseRange& range, std::size_t slots) noexcept {
    CheckZeroBlock();
    const std::lock_guard pack_lock{pack_mutex};
    holding_pack = true;
    struct Held { ~Held() { holding_pack = false; } } held;
    // Room made once: a pass allocates nothing (the heap may be out of memory when it runs).
    static std::vector<std::size_t> candidates;
    if (cold_hash.size() != slots) {
        cold_hash.assign(slots, 0);
        cold_since.assign(slots, ColdNow());
        candidates.clear();
        candidates.reserve(slots);
        pack_ready.reserve(slots);
    }
    candidates.clear();
    std::uint64_t own = 0, c1 = 0, c3 = 0, c6 = 0, zero = 0, zero_old = 0;
    const std::uint32_t now = ColdNow();
    const bool idle_mode = IdleEnabled() && PackEnabled() && idle_since != nullptr;
    const bool pool_short = idle_mode && !pack_refused.load(std::memory_order_relaxed) && PoolShortForPacking();
    std::size_t made_idle = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        const std::uint8_t state = SlotState(range, slot);
        if (state == SlotIdle) {
            // Untouched since it was made Idle (no hash: reading it would wake it).
            ++own;
            if (idle_mode && now - idle_since[slot] >= IdleSeconds()) candidates.push_back(slot);
            continue;
        }
        if (state != SlotOwn && state != SlotClean) {
            // (A slot in units keeps its hash here: UnitSweep, at the end of the pass, compares it.)
            if (const PackedSlot* const packed = packed_slots.load(std::memory_order_acquire);
                state == SlotPacked && packed != nullptr && packed[slot].units)
                continue;
            cold_since[slot] = now;
            cold_hash[slot] = 0;
            continue;
        }
        ++own;
        std::uint32_t age = 0;
        if (state == SlotOwn) {
            const std::uint64_t hash = ColdHash(SlotAddress(range, slot));
            // (A write that dropped the slot's copy in the swap store was counted there.)
            const bool noted = rewrite_noted != nullptr && rewrite_noted[slot].exchange(0, std::memory_order_relaxed) != 0;
            if (hash != cold_hash[slot]) {
                // Written again after a while unwritten: memory the game reuses (swap_rewritten).
                if (!noted && cold_hash[slot] != 0 && now - cold_since[slot] >= RecycleQuietSeconds) NoteRewritten(slot);
                cold_since[slot] = now;
                cold_hash[slot] = hash;
                if (pack_raw != nullptr) pack_raw[slot] = 0; // written: it may compress now
                if (unit_poor != nullptr) unit_poor[slot] = 0;
                if (swap_absent != nullptr) swap_absent[slot] = 0;
            }
            age = now - cold_since[slot];
            c1 += age >= 20;
            c3 += age >= 60;
            c6 += age >= 120;
        }
        // While the pool is short: no access for a while, to learn whether anything touches it.
        if (pool_short && made_idle < IdlePerPass && ProbeSlot(range, slot, now)) {
            ++made_idle;
            continue;
        }
        if (state != SlotOwn) continue;
        if (!idle_mode && age >= ColdSeconds) candidates.push_back(slot);
        if (ColdReport() && ColdLooksZero(SlotAddress(range, slot))) {
            ++zero;
            zero_old += age >= 60;
        }
    }
    cold_zero.store(zero, std::memory_order_relaxed);
    cold_zero_old.store(zero_old, std::memory_order_relaxed);
    cold_own.store(own, std::memory_order_relaxed);
    cold_1.store(c1, std::memory_order_relaxed);
    cold_3.store(c3, std::memory_order_relaxed);
    cold_6.store(c6, std::memory_order_relaxed);
    if (PackEnabled()) PackPass(range, slots, candidates);
    if (!ColdReport()) return;
    const auto start = std::chrono::steady_clock::now();
    const bool can_compress = &::Common::Compression::CompressDataLZ4 != nullptr;
    for (std::size_t i = 0; can_compress && i < std::min(ColdSamples, candidates.size()); ++i) {
        const std::size_t slot = candidates[(cold_rotor + i * 7919) % candidates.size()];
        if (SlotState(range, slot) != SlotOwn) continue; // packed meanwhile: a read would unpack it
        std::vector<std::uint8_t> copy(SparseSlot);
        std::memcpy(copy.data(), reinterpret_cast<const void*>(SlotAddress(range, slot)), SparseSlot);
        const auto packed = ::Common::Compression::CompressDataLZ4(copy.data(), copy.size());
        cold_sampled_bytes.fetch_add(SparseSlot, std::memory_order_relaxed);
        cold_compressed_bytes.fetch_add(packed.empty() ? SparseSlot : packed.size(), std::memory_order_relaxed);
    }
    cold_rotor += ColdSamples;
    cold_compress_ns.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
    const std::uint64_t sampled = cold_sampled_bytes.load(std::memory_order_relaxed);
    char line[400];
    const int length = std::snprintf(line, sizeof(line),
        "EDEN_CRAM_COLD own=%lluMiB unwritten_20s=%lluMiB unwritten_60s=%lluMiB unwritten_120s=%lluMiB "
        "lz4_sampled=%lluMiB lz4_ratio=%.3f lz4_ms=%llu zero_sampled=%lluMiB zero_unwritten_60s=%lluMiB\n",
        (unsigned long long)(own * 2), (unsigned long long)(c1 * 2), (unsigned long long)(c3 * 2),
        (unsigned long long)(c6 * 2), (unsigned long long)(sampled >> 20),
        sampled ? double(cold_compressed_bytes.load(std::memory_order_relaxed)) / double(sampled) : 0.0,
        (unsigned long long)(cold_compress_ns.load(std::memory_order_relaxed) / 1000000),
        (unsigned long long)(cold_zero.load(std::memory_order_relaxed) * 2),
        (unsigned long long)(cold_zero_old.load(std::memory_order_relaxed) * 2));
    if (length > 0) Report(line);
}

#ifdef PS5_NATIVE
// The packed store's blocks are filled one after another, and a block goes back only when all of
// its data did. Slots are unpacked in no order, so blocks stay held for the little that is left in
// them: ten minutes into a Mario Kart World session (3,900 slots packed, 2,700 unpacked again) the
// store held 544 MiB of blocks for 282 MiB of data. When 64 MiB or more of the store is such empty
// space, the data in blocks less than half full moves to the block being filled (a copy under the
// slot's lock; the slot stays Packed), and the emptied blocks go back. pack_mutex held.
std::atomic<std::uint64_t> store_compacted{0};
void StoreCompact(SparseRange& range, std::size_t slots) noexcept {
    PackedSlot* const table = packed_slots.load(std::memory_order_acquire);
    const std::size_t held = store_held.load(std::memory_order_relaxed);
    const std::size_t live = static_cast<std::size_t>(packed_bytes.load(std::memory_order_relaxed));
    static const bool on = EnvOr("EDEN_CRAM_STORE_COMPACT", 1) != 0;
    if (!on || table == nullptr || store_base == 0 || held < live + (std::size_t{64} << 20)) return;
    std::size_t moved = 0;
    for (std::size_t slot = 0; slot < slots && moved < (std::size_t{128} << 20); ++slot) {
        if (SlotState(range, slot) != SlotPacked) continue;
        void* const data = table[slot].data; // (looked at again under the slot's lock)
        if (data == nullptr) continue;        // in the swap store
        const std::size_t block = (reinterpret_cast<std::uintptr_t>(data) - store_base) / StoreBlock;
        if (block == store_current || store_live[block].load(std::memory_order_relaxed) * 2 > StoreBlock) continue;
        if (!TryLockSlot(range, slot)) continue; // (a fault handler has it: TryLockSlot)
        const std::uint8_t state = SlotState(range, slot);
        bool room = true;
        if (state == SlotPacked && table[slot].data == data) {
            const std::uint32_t size = table[slot].size;
            void* const fresh = StoreAlloc(size, nullptr);
            room = fresh != nullptr;
            if (room) {
                std::memcpy(fresh, data, size);
                table[slot].data = fresh;
                StoreRelease(data, size);
                moved += size;
            }
        }
        UnlockSlot(range, slot, state);
        if (!room) break; // the pool has no block for the store now
    }
    if (moved != 0) {
        store_compacted.fetch_add(moved, std::memory_order_relaxed);
        StoreReclaim();
    }
}
#endif

std::uint64_t StoreMoved() noexcept {
#ifdef PS5_NATIVE
    return store_compacted.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

void PackPass(SparseRange& range, std::size_t slots, std::vector<std::size_t>& candidates) noexcept {
#ifdef PS5_NATIVE
    StoreReclaim();
    StoreCompact(range, slots);
#endif
    if (packed_slots.load(std::memory_order_acquire) == nullptr) {
        auto* table = new (std::nothrow) PackedSlot[slots]();
        if (table == nullptr) return;
        if (gpu_touched == nullptr) {
            gpu_touched = new (std::nothrow) std::atomic<std::uint8_t>[slots]();
            if (gpu_cached_slots == 0) gpu_cached_slots = slots;
        }
        if (PackKeep()) {
            pack_kept = new (std::nothrow) std::int64_t[slots];
            if (pack_kept != nullptr)
                for (std::size_t i = 0; i < slots; ++i) pack_kept[i] = -1;
        }
        if (idle_since == nullptr && IdleEnabled()) {
            auto* since = new (std::nothrow) std::uint32_t[slots]();
            auto* touched = new (std::nothrow) std::atomic<std::uint32_t>[slots]();
            auto* times = new (std::nothrow) std::atomic<std::uint8_t>[slots]();
            if (since != nullptr && touched != nullptr && times != nullptr) {
                touch_times = times;
                touch_since = touched;
                idle_since = since;
            }
        }
        if (pack_raw == nullptr) pack_raw = new (std::nothrow) std::uint8_t[slots]();
        if (swap_absent == nullptr) swap_absent = new (std::nothrow) std::uint8_t[slots]();
        if (swap_units == nullptr) swap_units = new (std::nothrow) std::uint32_t[slots * SwapUnits]();
        if (swap_rewritten == nullptr) swap_rewritten = new (std::nothrow) std::atomic<std::uint8_t>[slots]();
        if (rewrite_noted == nullptr) rewrite_noted = new (std::nothrow) std::atomic<std::uint8_t>[slots]();
        if (swap_age_class == nullptr) swap_age_class = new (std::nothrow) std::uint8_t[slots]();
#ifdef PS5_NATIVE
        // Slots in units ("Units"): their tables, the slabs' address space and zstd's contexts.
        if (UnitsWanted()) {
            if (unit_slots == nullptr) unit_slots = new (std::nothrow) UnitSlot[slots]();
            if (unit_bytes == nullptr) unit_bytes = new (std::nothrow) std::uint16_t[slots * SwapUnits]();
            if (unit_poor == nullptr) unit_poor = new (std::nothrow) std::uint8_t[slots]();
            if (unit_scratch == nullptr) unit_scratch = new (std::nothrow) std::uint8_t[SparseSlot];
            if (unit_stage == nullptr) unit_stage = new (std::nothrow) std::uint8_t[SparseSlot];
            (void)UnitZstdStart();
            (void)SlabStart();
        }
#endif
        packed_slots.store(table, std::memory_order_release);
        pack_times.assign(slots, 0);
        recover_order.reserve(slots);
        pack_scratch.resize(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(SparseSlot))));
    }
    if (pack_refused.load(std::memory_order_relaxed) || !PoolShortForPacking()) return;
    // Device memory nothing is bound to goes back before any guest memory is packed.
    if (&eden_radv_private_radv_ps5_eden_trim != nullptr) (void)eden_radv_private_radv_ps5_eden_trim(std::uint64_t{512} << 20);
    // The pool is short: the swap store is opened (and what its storage has free looked at again).
    if (IdleEnabled() && idle_since != nullptr && SwapStart()) SwapRoom();
    // The longest idle first; without Idle slots the coldest, each needing ColdSeconds << (times packed) unwritten.
    const bool idle_mode = IdleEnabled() && idle_since != nullptr;
    const std::uint32_t now = ColdNow();
    std::vector<std::size_t>& ready = pack_ready;
    ready.clear();
    for (const std::size_t slot : candidates)
        if (idle_mode || now - cold_since[slot] >= (ColdSeconds << std::min<unsigned>(pack_times[slot], 4))) ready.push_back(slot);
    if (idle_mode) std::sort(ready.begin(), ready.end(), [](std::size_t a, std::size_t b) { return idle_since[a] < idle_since[b]; });
    else std::sort(ready.begin(), ready.end(), [](std::size_t a, std::size_t b) { return cold_since[a] < cold_since[b]; });
    // Into the packed store. What will not pack is left to the requests for memory (RecoverMemory),
    // which send it to the swap store when an allocation has failed.
    //
    // PS5_Lichii's pass sends it there itself, before any allocation fails (EDEN_SWAP_PASS=2 does
    // that here: slots never written again after a while unwritten with writes, inside the budget's
    // part that is not kept for failed allocations, the others only where the store has their bytes).
    // Its units are 64 KiB and what a touch brings back is one of them. Here a touch brings back 2 MiB,
    // and Mario Kart World writes most of its memory again within minutes: of 824 slots the pass
    // wrote in a 22-minute session the game had written 711 again, the budget was spent on them
    // (1.9 GiB), and the load that then needed memory got none and ended the session; so did the
    // next session, after 29 minutes. Ocarina of Time's memory stays as it is: of the 501 MiB one
    // session wrote there, the next found 480 again.
    // EDEN_SWAP_PASS=1: the pass sends only what costs no write, slots whose bytes the store holds.
    static const std::size_t pass_swaps = EnvOr("EDEN_SWAP_PASS", 0);
    std::size_t done = 0;
    bool dry = false; // the budget or the store's room said no in this pass
    for (const std::size_t slot : ready) {
        if (done >= PackPerPass) break;
        if (pack_times[slot] < 255) ++pack_times[slot];
        swap_refused = false;
        const Swap swap = pass_swaps == 0 ? Swap::No : pass_swaps >= 2 && Rewritten(slot) == 0 && !dry ? Swap::Yes : Swap::Reuse;
        if (PackSlot(range, slot, swap)) {
            ++done;
            cold_since[slot] = now;
        }
        dry = dry || swap_refused;
    }
#ifdef PS5_NATIVE
    // Slots in units: units still idle go, and those left alone for their time lose their access.
    swap_refused = false;
    (void)UnitSweep(range, now, IdleSeconds(), true, pass_swaps >= 2 && !dry ? SwapWrites::Budget : SwapWrites::None, 0);
#endif
    static const bool pass_log = EnvOr("EDEN_SWAP_LOG", 0) != 0;
    if (pass_log) {
        char line[240];
        const int length = std::snprintf(line, sizeof(line),
            "EDEN_SWAP_PASS free=%zuMiB packed=%zu store=%lluMiB written=%lluMiB found=%lluMiB tokens=%lldMiB\n",
            PoolFreePieces() >> 20, done, (unsigned long long)(std::uint64_t{swap_count} * SwapUnit >> 20),
            (unsigned long long)(swap_written.load(std::memory_order_relaxed) >> 20),
            (unsigned long long)(swap_found.load(std::memory_order_relaxed) * SwapUnit >> 20), (long long)(swap_tokens >> 20));
        if (length > 0) Report(line);
    }
}

void ScanPass(SparseRange& range) noexcept {
    const auto start = std::chrono::steady_clock::now();
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    std::vector<std::pair<std::uint64_t, std::size_t>> samples;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (SlotState(range, slot) != SlotOwn) continue;
        const std::uintptr_t at = SlotAddress(range, slot);
        if (AllZero(at)) { // a read without the lock: only a hint, checked again fenced
            if (ScanZero(range, slot)) counters.returns_scan.fetch_add(1, std::memory_order_relaxed);
        } else if (fine_requested.load(std::memory_order_relaxed) && ScanFine(range, slot)) {
            // now Fine
        } else if (dedup_requested.load(std::memory_order_relaxed)) {
            samples.emplace_back(SampleHash(at), slot);
        }
    }
    std::sort(samples.begin(), samples.end());
    for (std::size_t i = 0; i < samples.size();) {
        std::size_t j = i + 1;
        while (j < samples.size() && samples[j].first == samples[i].first) ++j;
        if (j - i > 1) {
            std::vector<std::pair<std::uint64_t, std::size_t>> full;
            for (std::size_t k = i; k < j; ++k)
                full.emplace_back(FullHash(SlotAddress(range, samples[k].second), SparseSlot), samples[k].second);
            std::sort(full.begin(), full.end());
            for (std::size_t a = 0; a < full.size();) {
                std::size_t b = a + 1;
                while (b < full.size() && full[b].first == full[a].first) {
                    (void)ScanShare(range, full[a].second, full[b].second);
                    ++b;
                }
                a = b;
            }
        }
        i = j;
    }
    if (ColdReport() || PackEnabled()) ColdPass(range, slots);
    counters.passes.fetch_add(1, std::memory_order_relaxed);
    counters.scan_ns.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
}

void ServiceReport(SparseRange& range, const char* when) noexcept {
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    std::size_t own = 0, shared = 0, fine = 0, fine_kept = 0, idle = 0, clean = 0, packed = 0, swapped_out = 0;
    std::size_t in_units = 0, units_here = 0, units_idle = 0;
    std::size_t kinds[4] = {}; // slots that hold data, by how often they were written again (swap_rewritten)
    PackedSlot* const packed_table = packed_slots.load(std::memory_order_acquire);
    for (std::size_t slot = 0; slot < slots; ++slot) {
        const auto state = SlotState(range, slot);
        if (&range == guest_range.load(std::memory_order_acquire) &&
            (state == SlotOwn || state == SlotIdle || state == SlotClean || state == SlotPacked))
            ++kinds[std::min<unsigned>(Rewritten(slot), 3)];
        own += state == SlotOwn || state == SlotIdle || state == SlotClean; // a block of its own, as Own
        idle += state == SlotIdle;
        clean += state == SlotClean;
        packed += state == SlotPacked;
        swapped_out += state == SlotPacked && packed_table != nullptr && packed_table[slot].swap != 0;
        if (state == SlotPacked && packed_table != nullptr && packed_table[slot].units && unit_slots != nullptr) {
            ++in_units;
            units_here += static_cast<std::size_t>(__builtin_popcount(unit_slots[slot].here));
            units_idle += static_cast<std::size_t>(__builtin_popcount(unit_slots[slot].idle));
        }
        shared += state == SlotShared;
        if (state == SlotFine) {
            ++fine;
            fine_kept += FineKept(range, slot);
        }
    }
    char line[640];
    const int length = std::snprintf(line, sizeof(line),
        "EDEN_CRAM %s held=%zuMiB own=%zu shared=%zu zero=%zu faults=%llu promotions=%llu copies=%llu waits=%llu "
        "returns_clear=%llu returns_free=%llu returns_scan=%llu dedup=%llu reserve_used=%llu failures=%llu "
        "passes=%llu scan_ms=%llu fine=%zu fine_held=%zuMiB fined=%llu unfined=%llu fine_failures=%llu "
        "reserve=%zuMiB host_transferred=%lluMiB freed_parts=%llu freed_parts_cleared=%lluMiB\n",
        when, guest_committed.load(std::memory_order_relaxed) >> 20, own, shared, slots - own - shared - fine - packed,
        (unsigned long long)counters.faults.load(), (unsigned long long)counters.promotions.load(),
        (unsigned long long)counters.copies.load(), (unsigned long long)counters.waits.load(),
        (unsigned long long)counters.returns_clear.load(), (unsigned long long)counters.returns_free.load(),
        (unsigned long long)counters.returns_scan.load(), (unsigned long long)counters.dedup.load(),
        (unsigned long long)counters.reserve_used.load(), (unsigned long long)counters.failures.load(),
        (unsigned long long)counters.passes.load(), (unsigned long long)(counters.scan_ns.load() / 1000000),
        fine, fine_kept * FinePage >> 20, (unsigned long long)counters.fined.load(),
        (unsigned long long)counters.unfined.load(), (unsigned long long)counters.fine_failures.load(),
        reserve_count.load(std::memory_order_relaxed) * (SparseSlot >> 20),
        (unsigned long long)counters.host_reserve_used.load() * (SparseSlot >> 20),
        (unsigned long long)freed_part_calls.load(), (unsigned long long)(freed_part_bytes.load() >> 20));
    if (length > 0) Report(line);
    // Who wrote the slots that were promoted: the largest places (offsets in the app's image).
    struct Top { const WriterSite* site; std::uint64_t count; } top[10]{};
    for (const WriterSite& site : writer_sites) {
        const std::uint64_t count = site.count.load(std::memory_order_relaxed);
        if (site.key.load(std::memory_order_relaxed) == 0 || count <= top[9].count) continue;
        std::size_t place = 9;
        while (place > 0 && top[place - 1].count < count) {
            top[place] = top[place - 1];
            --place;
        }
        top[place] = {&site, count};
    }
    char writers[640];
    int used = std::snprintf(writers, sizeof(writers),
                             "EDEN_CRAM_WRITERS promotions by code of the app %llu, by other code %llu\n",
                             (unsigned long long)promotions_in_app_code.load(), (unsigned long long)promotions_elsewhere.load());
    if (used > 0) Report(writers);
    for (const Top& entry : top) {
        if (entry.count == 0) break;
        int length = std::snprintf(writers, sizeof(writers), "EDEN_CRAM_WRITER %llu:", (unsigned long long)entry.count);
        for (const std::uintptr_t pc : entry.site->pcs) {
            if (length > 0 && static_cast<std::size_t>(length) < sizeof(writers) - 24)
                length += std::snprintf(writers + length, sizeof(writers) - length, " %llx",
                                        (unsigned long long)(pc != 0 ? pc - 0x400000ull : 0));
        }
        if (length > 0 && static_cast<std::size_t>(length) < sizeof(writers) - 2) {
            writers[length++] = '\n';
            writers[length] = 0;
            Report(writers);
        }
    }
    if (PackEnabled()) {
        char pack_line[400];
        const int pack_length = std::snprintf(pack_line, sizeof(pack_line),
            "EDEN_CRAM_PACK %s packed=%lluMiB stored=%lluMiB store_held=%lluMiB store_moved=%lluMiB packs=%llu unpacks=%llu skips=%llu unpack_keep=%lluMiB"
            " kept_compared=%llu kept_differed=%llu zero_block_dirty=%llu gpu_skips=%llu gpu_marked=%llu\n", when,
            (unsigned long long)(packed_count.load(std::memory_order_relaxed) * (SparseSlot >> 20)),
            (unsigned long long)(packed_bytes.load(std::memory_order_relaxed) >> 20),
            (unsigned long long)(store_held.load(std::memory_order_relaxed) >> 20),
            (unsigned long long)(StoreMoved() >> 20),
            (unsigned long long)packs.load(std::memory_order_relaxed), (unsigned long long)unpacks.load(std::memory_order_relaxed),
            (unsigned long long)pack_skips.load(std::memory_order_relaxed), (unsigned long long)(unpack_keep_count.load(std::memory_order_relaxed) * 2),
            (unsigned long long)pack_keep_compared.load(std::memory_order_relaxed),
            (unsigned long long)pack_keep_differed.load(std::memory_order_relaxed),
            (unsigned long long)zero_block_dirty.load(std::memory_order_relaxed),
            (unsigned long long)pack_gpu_skips.load(std::memory_order_relaxed),
            (unsigned long long)pack_gpu_marked.load(std::memory_order_relaxed));
        if (pack_length > 0) Report(pack_line);
        ReportUnpackers();
        // The swap store and the Idle slots. written: what this session added to the store (what
        // wears the storage); found: what went there without a write, its bytes being there already.
        const std::uint64_t reads = swap_reads.load(std::memory_order_relaxed);
        const std::uint64_t written = swap_written.load(std::memory_order_relaxed);
        const std::uint64_t writes = written / SwapUnit;
        char swap_line[640];
        const int swap_length = std::snprintf(swap_line, sizeof(swap_line),
            "EDEN_SWAP %s on=%d (%s) store=%lluMiB out=%zuMiB clean=%zuMiB written=%lluMiB found=%lluMiB zeroes=%lluMiB "
            "write_ms=%llu writes=%llu reused=%llu dirtied=%llu reads=%llu read_avg_us=%llu read_max_us=%llu denied_rate=%llu full=%llu "
            "absent=%llu idle_now=%zuMiB idled=%llu woken=%llu kept_free=%lluMiB/%llu last_resort=%llu late_reads=%llu/%llu\n",
            when, swap_state.load(std::memory_order_relaxed) == 1 ? 1 : 0,
            swap_state.load(std::memory_order_relaxed) == 1 ? "ok" : swap_off_why.load(std::memory_order_acquire),
            (unsigned long long)(std::uint64_t{swap_count} * SwapUnit >> 20), swapped_out * (SparseSlot >> 20),
            clean * (SparseSlot >> 20), (unsigned long long)(written >> 20),
            (unsigned long long)(swap_found.load(std::memory_order_relaxed) * SwapUnit >> 20),
            (unsigned long long)(swap_zero_units.load(std::memory_order_relaxed) * SwapUnit >> 20),
            (unsigned long long)(swap_write_ns.load(std::memory_order_relaxed) / 1000000),
            (unsigned long long)swap_appends.load(std::memory_order_relaxed),
            (unsigned long long)swap_reused.load(std::memory_order_relaxed),
            (unsigned long long)swap_dirtied.load(std::memory_order_relaxed), (unsigned long long)reads,
            (unsigned long long)(reads ? swap_read_ns.load(std::memory_order_relaxed) / reads / 1000 : 0),
            (unsigned long long)(swap_read_max_ns.load(std::memory_order_relaxed) / 1000),
            (unsigned long long)swap_denied_rate.load(std::memory_order_relaxed),
            (unsigned long long)swap_full.load(std::memory_order_relaxed),
            (unsigned long long)swap_missed.load(std::memory_order_relaxed), idle * (SparseSlot >> 20),
            (unsigned long long)idled.load(std::memory_order_relaxed), (unsigned long long)idle_woken.load(std::memory_order_relaxed),
            (unsigned long long)(keeper_bytes.load(std::memory_order_relaxed) >> 20),
            (unsigned long long)keeper_calls.load(std::memory_order_relaxed),
            (unsigned long long)swap_last_resort.load(std::memory_order_relaxed),
            (unsigned long long)clean_late_reads.load(std::memory_order_relaxed),
            (unsigned long long)clean_refaults.load(std::memory_order_relaxed));
        if (swap_length > 0) Report(swap_line);
#ifdef PS5_NATIVE
        // Slots in units: how many, what their units hold in memory (and how much of it without
        // access), what the slabs hold for them, and what became of units so far.
        const std::uint64_t backs = unit_backs.load(std::memory_order_relaxed);
        const int unit_length = std::snprintf(swap_line, sizeof(swap_line),
            "EDEN_UNITS %s on=%d slots=%zu here=%zuMiB idle=%zuMiB slabs=%zuMiB made=%llu packed=%llu stayed=%llu "
            "backs=%llu back_avg_us=%llu zero_reads=%llu woken=%llu evicted=%llu moved=%llu whole=%llu\n",
            when, UnitsReady() ? 1 : 0, in_units, units_here * SwapUnit >> 20, units_idle * SwapUnit >> 20,
            slab_held.load(std::memory_order_relaxed) >> 20, (unsigned long long)unit_slots_made.load(std::memory_order_relaxed),
            (unsigned long long)unit_packed.load(std::memory_order_relaxed),
            (unsigned long long)unit_stayed.load(std::memory_order_relaxed), (unsigned long long)backs,
            (unsigned long long)(backs ? unit_back_ns.load(std::memory_order_relaxed) / backs / 1000 : 0),
            (unsigned long long)unit_zero_reads.load(std::memory_order_relaxed),
            (unsigned long long)unit_woken.load(std::memory_order_relaxed),
            (unsigned long long)unit_evicted.load(std::memory_order_relaxed),
            (unsigned long long)unit_moved.load(std::memory_order_relaxed),
            (unsigned long long)unit_whole.load(std::memory_order_relaxed));
        if (unit_length > 0) Report(swap_line);
#endif
        // Slots written by how long they had been unwritten (under 30 s, 60 s, 2, 5, 10 min, more)
        // and by how often they had been written again before (never, once, twice, more), with how
        // many of each a later write dropped; and the slots that hold data now, by the same count.
        const int ages_length = std::snprintf(swap_line, sizeof(swap_line),
            "EDEN_SWAP_AGES %s written=%u/%u/%u/%u/%u/%u dropped=%u/%u/%u/%u/%u/%u by_rewrites=%u/%u/%u/%u "
            "dropped=%u/%u/%u/%u slots=%zu/%zu/%zu/%zu\n", when,
            swap_age_written[0].load(), swap_age_written[1].load(), swap_age_written[2].load(), swap_age_written[3].load(),
            swap_age_written[4].load(), swap_age_written[5].load(), swap_age_dropped[0].load(), swap_age_dropped[1].load(),
            swap_age_dropped[2].load(), swap_age_dropped[3].load(), swap_age_dropped[4].load(), swap_age_dropped[5].load(),
            swap_kind_written[0].load(), swap_kind_written[1].load(), swap_kind_written[2].load(), swap_kind_written[3].load(),
            swap_kind_dropped[0].load(), swap_kind_dropped[1].load(), swap_kind_dropped[2].load(), swap_kind_dropped[3].load(),
            kinds[0], kinds[1], kinds[2], kinds[3]);
        if (ages_length > 0 && (writes != 0 || kinds[1] + kinds[2] + kinds[3] != 0)) Report(swap_line);
    }
}

// What finer slots would give: 16 KiB pages of the Own slots that hold only zeroes, and those that
// repeat another page (by a full hash). Read once, when the session ends.
void MeasureFinePages(SparseRange& range) noexcept {
    constexpr std::size_t Fine = 16384;
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    std::vector<std::uint64_t> hashes;
    std::size_t pages = 0, zero = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (SlotState(range, slot) != SlotOwn) continue;
        for (std::size_t offset = 0; offset < SparseSlot; offset += Fine) {
            const std::uintptr_t at = SlotAddress(range, slot) + offset;
            ++pages;
            const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
            bool is_zero = true;
            for (std::size_t i = 0; i < Fine / 8 && is_zero; ++i) is_zero = word[i] == 0;
            if (is_zero) ++zero;
            else hashes.push_back(FullHash(at, Fine));
        }
    }
    std::sort(hashes.begin(), hashes.end());
    std::size_t repeats = 0;
    for (std::size_t i = 1; i < hashes.size(); ++i) repeats += hashes[i] == hashes[i - 1];
    char line[192];
    const int length = std::snprintf(line, sizeof(line),
        "EDEN_CRAM_FINE pages16k=%zu zero=%zu (%zuMiB) repeats=%zu (%zuMiB)\n", pages, zero, zero * Fine >> 20, repeats,
        repeats * Fine >> 20);
    if (length > 0) Report(line);
}

void ScannerLoop(SparseRange* range) {
    auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::unique_lock lock{scanner_mutex};
    while (!scanner_stop) {
        // Every 20 s; every 5 s while packing has a short pool to answer (the game grows ~10 MiB/s).
        const bool hurry = PackEnabled() && !pack_refused.load(std::memory_order_relaxed) && PoolShortForPacking();
        // While the pool is not short it is looked at every 2 s (one system call), and the pass comes
        // at once when it runs short: a race load takes 1.5 GB within seconds, and the slots nothing
        // touches have to be found (Idle slots) before the memory is asked back.
        const int seconds = !hurry ? 20 : 5;
        for (int waited = 0; waited < seconds && !scanner_stop; waited += 2) {
            scanner_wake.wait_for(lock, std::chrono::seconds(std::min(2, seconds - waited)), [] { return scanner_stop; });
            if (!hurry && !scanner_stop && PackEnabled() && !pack_refused.load(std::memory_order_relaxed) && PoolShortForPacking())
                break;
        }
        if (scanner_stop) break;
        lock.unlock();
        if (scan_requested.load(std::memory_order_relaxed)) ScanPass(*range);
        if (std::chrono::steady_clock::now() >= next_report) {
            ServiceReport(*range, "now");
            next_report = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        }
        lock.lock();
    }
}

// Once: does a write to a slot that shows the zero block come back here, with its address, and
// resume? A refusal keeps the guest's memory committed when it is handed out, as before.
bool ServiceAvailable() noexcept {
    int state = service_state.load(std::memory_order_acquire);
    if (state != 0) return state == 1;
    const char* step = "ok";
    long long fault_ns = -1;
    void* base = nullptr;
    if (!SparseAvailable()) step = "sparse";
    else if (!InstallServiceSignal()) step = "install";
    else if ((base = NewSparseRange(2 * SparseSlot, false)) == nullptr) step = "reserve";
    if (base != nullptr) {
        SparseRange* range = nullptr;
        {
            const std::lock_guard lock{sparse_mutex};
            range = SparseRangeOf(reinterpret_cast<std::uintptr_t>(base));
        }
        if (range == nullptr || !AddStates(*range)) {
            step = "states";
        } else {
            probe_range.store(range, std::memory_order_release);
            auto* word = reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uint8_t*>(base) + SparseSlot + 64);
            const auto before = counters.faults.load(std::memory_order_relaxed);
            probe_expected.store(reinterpret_cast<std::uintptr_t>(word), std::memory_order_release);
            const auto start = std::chrono::steady_clock::now();
            *word = 0x5a17ed5a17ed5a17ull; // faults once: the slot shows the zero block
            fault_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            probe_expected.store(0, std::memory_order_release);
            if (*word != 0x5a17ed5a17ed5a17ull) step = "value";
            else if (counters.faults.load(std::memory_order_relaxed) != before + 1) step = "fault-count";
            else if (probe_mismatch.load(std::memory_order_relaxed)) step = "fault-address";
            else if (SlotState(*range, 1) != SlotOwn || SlotState(*range, 0) != SlotZero) step = "state";
            else {
                // Back to the zero block, then written again: a slot comes and goes.
                LockSlot(*range, 1);
                ReturnSlot(*range, 1);
                UnlockSlot(*range, 1, SlotZero);
                if (*word != 0) step = "return";
                else {
                    *word = 7;
                    if (*word != 7 || SlotState(*range, 1) != SlotOwn) step = "again";
                }
            }
            if (std::strcmp(step, "ok") == 0) {
                // Fenced and unfenced: read while fenced, written after.
                const std::uintptr_t at = SlotAddress(*range, 1);
                Fence(at);
                if (*word != 7) step = "fence-read";
                Unfence(at);
                *word = 9;
                if (std::strcmp(step, "ok") == 0 && *word != 9) step = "unfence";
            }
            if (std::strcmp(step, "ok") == 0) {
                // Idle: no access until a read wakes it, with its data as it was, writable again.
                LockSlot(*range, 1);
                const bool idle = MakeIdle(*range, 1, 0);
                UnlockSlot(*range, 1, idle ? SlotIdle : SlotOwn);
                const auto before_wake = counters.faults.load(std::memory_order_relaxed);
                if (!idle) step = "idle";
                else if (*word != 9) step = "idle-read";
                else if (SlotState(*range, 1) != SlotOwn || counters.faults.load(std::memory_order_relaxed) != before_wake + 1)
                    step = "idle-wake";
                else {
                    *word = 9; // written again without a fault
                    if (counters.faults.load(std::memory_order_relaxed) != before_wake + 1) step = "idle-write";
                }
            }
            if (std::strcmp(step, "ok") == 0) {
                // Two slots of the same bytes: one block, then copied apart by a write, then the last
                // one takes the block back.
                auto* other = reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uint8_t*>(base) + 64);
                *other = 9;
                const bool shared = ScanShare(*range, 1, 0);
#ifdef PS5_NATIVE
                if (!shared || SlotState(*range, 0) != SlotShared || SlotState(*range, 1) != SlotShared) step = "share";
                else if (*other != 9 || *word != 9) step = "share-read";
                else {
                    *other = 10; // a copy of its own
                    if (*other != 10 || *word != 9 || SlotState(*range, 0) != SlotOwn) step = "share-copy";
                    else {
                        *word = 11; // the last one: the block is its own again
                        if (*word != 11 || *other != 10 || SlotState(*range, 1) != SlotOwn) step = "share-last";
                    }
                }
#else
                if (shared) step = "share-host";
#endif
            }
            if (std::strcmp(step, "ok") == 0) {
                // Slot 1 with one page of data at page 3: Fine (one 16 KiB page kept), read in place,
                // then a write makes it whole again with every byte where it was.
                LockSlot(*range, 1);
                if (SlotState(*range, 1) != SlotZero) ReturnSlot(*range, 1);
                UnlockSlot(*range, 1, SlotZero);
                auto* page3 = reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uint8_t*>(base) + SparseSlot + 3 * FinePage + 8);
                *page3 = 0x0f1e2d3c4b5a6978ull;
                const auto held = guest_committed.load(std::memory_order_relaxed);
                if (!ScanFine(*range, 1) || SlotState(*range, 1) != SlotFine) step = "fine";
                else if (*page3 != 0x0f1e2d3c4b5a6978ull || *word != 0) step = "fine-read";
                else if (held - guest_committed.load(std::memory_order_relaxed) != SparseSlot - FinePage) step = "fine-held";
                else {
                    *word = 13; // page 0: the slot is whole again
                    if (*word != 13 || *page3 != 0x0f1e2d3c4b5a6978ull || SlotState(*range, 1) != SlotOwn ||
                        (range->fine[1] & 0xf00) == 0)
                        step = "fine-write";
                }
            }
            for (std::size_t slot = 0; slot < 2; ++slot)
                if (SlotState(*range, slot) != SlotZero) {
                    LockSlot(*range, slot);
                    ReturnSlot(*range, slot);
                    UnlockSlot(*range, slot, SlotZero);
                }
            probe_range.store(nullptr, std::memory_order_release);
        }
        FreeSparsePages(base, 2 * SparseSlot);
    }
    ResetCounters();
    const bool ok = std::strcmp(step, "ok") == 0;
    service_refused = step;
    char line[128];
    const int length = std::snprintf(line, sizeof(line), "EDEN_CRAM_CHECK ok=%d step=%s fault_ns=%lld\n", ok ? 1 : 0,
                                     step, fault_ns);
    if (length > 0) Report(line);
    service_state.store(ok ? 1 : 2, std::memory_order_release);
    return ok;
}

// The guest range starts being a service (when the platform passed the check).
void ServiceStart(SparseRange& range) noexcept {
    // A refusal belongs to the session it ended: a later crash must not report it again.
    refused_address.store(0, std::memory_order_relaxed);
    if (!service_requested.load(std::memory_order_relaxed) || !ServiceAvailable() || !AddStates(range)) {
        char line[96];
        const int length = std::snprintf(line, sizeof(line), "EDEN_CRAM off: %s\n",
                                         !service_requested.load() ? "switched-off" : service_refused);
        if (length > 0) Report(line);
        return;
    }
    ResetCounters();
    range.promote = true;
#ifdef PS5_NATIVE
    FillReserve(ReserveAtStart);
#endif
    service_range.store(&range, std::memory_order_release);
    Eden::Fastmem::guest_write_fault.store(&HandleGuestWriteFault, std::memory_order_release);
    {
        const std::lock_guard lock{scanner_mutex};
        scanner_stop = false;
    }
    scanner = std::thread(ScannerLoop, &range);
#ifdef PS5_NATIVE
    keeper = std::thread(ReserveKeeper);
#endif
    char line[128];
    const int length = std::snprintf(line, sizeof(line), "EDEN_CRAM on: scan=%d dedup=%d fine=%d reserve=%zuMiB\n",
                                     scan_requested.load() ? 1 : 0, dedup_requested.load() ? 1 : 0,
                                     fine_requested.load() ? 1 : 0,
                                     reserve_count.load() * (SparseSlot >> 20));
    if (length > 0) Report(line);
}

void ServiceStop(SparseRange& range) noexcept {
    if (service_range.load(std::memory_order_acquire) != &range) return;
    {
        const std::lock_guard lock{scanner_mutex};
        scanner_stop = true;
    }
    scanner_wake.notify_all();
    if (scanner.joinable()) scanner.join();
#ifdef PS5_NATIVE
    if (keeper.joinable()) keeper.join();
#endif
    ServiceReport(range, "end");
    MeasureFinePages(range);
    service_range.store(nullptr, std::memory_order_release);
    // A request for memory that is at the range's slots now (the driver's or the heap's, on a thread
    // of its own) holds pack_mutex: it is waited for here, and those that come later find no range
    // (RecoverMemory). The tables it reads go after this (ReleaseServiceBlocks).
    if (!holding_pack) {
        const std::lock_guard pack_lock{pack_mutex};
    }
    // Packed data goes with the range (ReleaseServiceBlocks drops what is still packed after this).
#ifdef PS5_NATIVE
    EmptyReserve();
#endif
}

// What a guest range keeps when it is given back: each block once, shared ones too.
void ReleaseServiceBlocks(SparseRange& range) noexcept {
#ifdef PS5_NATIVE
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        DropSlotAliases(range, slot);
        const auto state = SlotState(range, slot);
        if (state == SlotFine) {
            const std::size_t kept = FineKept(range, slot);
            if (kept != 0 && sceKernelReleaseDirectMemory(range.physical[slot], kept * FinePage) != 0) std::abort();
            guest_committed.fetch_sub(kept * FinePage, std::memory_order_relaxed);
        } else if (state == SlotOwn || state == SlotIdle || state == SlotClean) {
            if (state != SlotOwn && range.guest) ForgetPacked(slot); // (guest_range is cleared by now)
            if (sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
        } else if (state == SlotPacked) {
            ForgetPacked(slot);
        } else if (state == SlotShared && UnshareBlock(range.physical[slot]) == 0) {
            if (sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
            guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
        }
        if (state == SlotOwn || state == SlotIdle || state == SlotClean) guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
        range.owned[slot].store(false, std::memory_order_relaxed);
    }
    if (&range == guest_range.load(std::memory_order_acquire) || service_range.load(std::memory_order_acquire) == nullptr) {
        store_current = StoreBlocks; // every packed slot was forgotten above: all blocks go back
        StoreReclaim();
        delete[] packed_slots.exchange(nullptr, std::memory_order_acq_rel);
        cold_hash.clear();
        cold_since.clear();
        pack_times.clear();
        // (The swap store stays as it is, open: its slots are found by their contents, by the next
        // game of this session and by later sessions.)
        swap_slots.store(0, std::memory_order_relaxed);
        delete[] std::exchange(swap_units, nullptr);
        delete[] std::exchange(swap_absent, nullptr);
        delete[] std::exchange(idle_since, nullptr);
        delete[] std::exchange(touch_since, nullptr);
        delete[] std::exchange(touch_times, nullptr);
        delete[] std::exchange(pack_raw, nullptr);
        delete[] std::exchange(swap_rewritten, nullptr);
        delete[] std::exchange(rewrite_noted, nullptr);
        delete[] std::exchange(swap_age_class, nullptr);
        delete[] std::exchange(unit_slots, nullptr);
        delete[] std::exchange(unit_bytes, nullptr);
        delete[] std::exchange(unit_poor, nullptr);
    }
#else
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (SlotState(range, slot) == SlotOwn) guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
        if (SlotState(range, slot) == SlotFine) guest_committed.fetch_sub(FineKept(range, slot) * FinePage, std::memory_order_relaxed);
        range.owned[slot].store(false, std::memory_order_relaxed);
    }
#endif
}
} // namespace

// The driver ran out of device memory (vulkan_memory_allocator.cpp): packs the coldest slots now,
// those unwritten for 10 s or more, until `bytes` went back. Returns what went back.
std::atomic<std::uint64_t> recover_calls{0}, recovered_bytes{0};
namespace {
#ifdef PS5_NATIVE
// The last resort of RecoverMemory: blocks the service holds idle (kept for unpacking, then the
// guest's reserve) go back to the pool. Late in a long session without Pro memory the heap and
// the JIT were refused their next piece and the app ended, while 768 MiB sat in these two; a
// guest write or unpack that later finds the pool empty asks for packing as before.
std::size_t GiveBackIdleBlocks(std::size_t bytes, bool quiet = false) noexcept {
    std::size_t freed = 0;
    while (freed < bytes) {
        std::int64_t physical = -1;
        Spin(unpack_keep_lock);
        if (const std::size_t count = unpack_keep_count.load(std::memory_order_relaxed); count > 0) {
            physical = unpack_keep[count - 1];
            unpack_keep_count.store(count - 1, std::memory_order_relaxed);
        }
        unpack_keep_lock.clear(std::memory_order_release);
        if (physical < 0) {
            Spin(reserve_lock);
            if (const std::size_t count = reserve_count.load(std::memory_order_relaxed); count > 0) {
                physical = reserve[count - 1];
                reserve_count.store(count - 1, std::memory_order_relaxed);
            }
            reserve_lock.clear(std::memory_order_release);
        }
        if (physical < 0) break;
        if (sceKernelReleaseDirectMemory(physical, SparseSlot) != 0) std::abort();
        freed += SparseSlot;
    }
    if (freed != 0) {
        reserve_hold_ns.store(SwapNow() + 2'000'000'000ull, std::memory_order_relaxed);
        idle_given_back.fetch_add(freed, std::memory_order_relaxed);
        if (idle_give_back_calls.fetch_add(1, std::memory_order_relaxed) < 24 && !quiet) {
            char line[96];
            const int length = std::snprintf(line, sizeof(line), "EDEN_CRAM idle blocks given back: %zu MiB\n", freed >> 20);
            if (length > 0) Report(line);
        }
    }
    return freed;
}
#endif
std::atomic<std::uint64_t> recover_short{0};
constexpr unsigned RecoverSlotBits = 23;      // a slot's number in an entry of recover_order
constexpr std::size_t LargeRequest = std::size_t{64} << 20; // see RecoverMemory
constexpr std::uint32_t KeeperColdSeconds = 60; // see RecoverMemory
// `driver`: the driver may be asked to trim (not from a fault handler: the thread may be in the driver).
// `keeper`: the keeper's own call (KeepFree): no memory was refused yet; it never waits for the lock,
// takes nothing from the blocks kept for the guest, and prints nothing. 1: it only packs; 2: it may
// write to the swap store too.
std::size_t RecoverMemory(std::size_t bytes, bool wait, bool driver = true, int keeper = 0) noexcept {
#ifdef PS5_NATIVE
    SparseRange* const range = service_range.load(std::memory_order_acquire);
    // Device memory no resource uses goes back first: it costs nothing later (packing makes the
    // guest fault its data back).
    std::size_t trimmed = 0;
    if (driver && &eden_radv_private_radv_ps5_eden_trim != nullptr)
        trimmed = static_cast<std::size_t>(eden_radv_private_radv_ps5_eden_trim(bytes));
    if (trimmed >= bytes) return trimmed;
    bytes -= trimmed;
    if (range == nullptr || !PackEnabled() || pack_refused.load(std::memory_order_relaxed) || holding_pack)
        return trimmed + (keeper ? 0 : GiveBackIdleBlocks(bytes, !driver));
    std::unique_lock pack_lock{pack_mutex, std::defer_lock};
    if (keeper) {
        if (!pack_lock.try_lock()) return trimmed; // a pass or a request is at it: again in 200 ms
    } else if (wait) {
        pack_lock.lock();
    } else {
        // The heap and the fault handlers: never a wait without end (whoever packs may be waiting for
        // the heap's lock), but up to half a second: a pass writing to the swap store holds the lock
        // that long, and the heap's growth that found it taken got nothing back and failed
        // (std::bad_alloc ended a Mario Kart World session with 1.3 GB of the budget unused).
        for (unsigned tries = 0; !pack_lock.try_lock(); ++tries) {
            if (tries == 500) return trimmed + GiveBackIdleBlocks(bytes, !driver);
            const timespec pause{0, 1'000'000};
            nanosleep(&pause, nullptr);
        }
    }
    holding_pack = true;
    struct Held { ~Held() { holding_pack = false; } } held;
    // (A game's end clears service_range and then takes this lock once, before the range's tables
    // go: ServiceStop. A request that was on its way here finds that out now.)
    if (service_range.load(std::memory_order_acquire) != range) return trimmed;
    if (packed_slots.load(std::memory_order_acquire) == nullptr || cold_since.empty() ||
        recover_order.capacity() < cold_since.size() || cold_since.size() >> RecoverSlotBits != 0)
        return trimmed + (keeper ? 0 : GiveBackIdleBlocks(bytes, !driver));
    // What is asked for is all the taker needs, and the pool may have part of it: the driver asks
    // for the whole 560 MiB of one image and 16 more (a texture array of Mario Kart World's, at a
    // track's load), again at each of its tries, and 574 MiB of the guest's memory gave their
    // blocks up each time, up to 250 MiB of them by way of the swap store, whatever the pool had
    // free. Only what the pool lacks is taken now (and no less than LargeRequest, for the takers
    // that come in between).
    const std::size_t pool_free = driver && !keeper ? PoolFreePieces() : 0;
    const std::size_t asked = bytes;
    if (bytes > LargeRequest && pool_free != 0 && pool_free < bytes) bytes = std::max(bytes - pool_free, LargeRequest);
    // Only slots nothing touches are taken: Idle for UrgentIdleSeconds or more, the longest idle
    // first. It used to take Own slots unwritten for 10 s as well, and when those ran out, for 3 s.
    // Unwritten is not unread: at a race start of Mario Kart World such slots were the game's own
    // working memory. Its threads faulted them back in at once (20,000 unpacks in two minutes, each
    // needing a block that the next request took from another such slot), the game held 2.4 GB of
    // the 4.4 GB it uses, and frames took three times as long for the rest of the race. A request
    // that finds no such slot now gets nothing, and its allocation waits or fails.
    // (A slot's state may change while this runs: PackSlot looks at it again under the slot's lock.)
    const std::uint32_t now = ColdNow();
    const bool idle_mode = IdleEnabled() && idle_since != nullptr;
    recover_order.clear();
    if (idle_mode) {
        // (The keeper, with no allocation failed yet, takes what the scanner's own passes would.)
        const std::size_t idle_for = keeper ? IdleSeconds() : UrgentIdleSeconds();
        for (std::size_t slot = 0; slot < cold_since.size(); ++slot)
            if (SlotState(*range, slot) == SlotIdle && now - idle_since[slot] >= idle_for)
                recover_order.push_back(slot);
        // Slots the swap store already has first (giving their blocks up writes nothing). Then those
        // that were never written again after a while unwritten, before memory the game reuses
        // (swap_rewritten), and of each kind the longest unwritten first. The order is fixed here,
        // from what the slots are now (fault handlers change them while this runs): each entry
        // carries its place, then the slot.
        PackedSlot* const in_file = packed_slots.load(std::memory_order_acquire);
        for (std::size_t& entry : recover_order) {
            const std::size_t slot = entry;
            entry = (in_file[slot].swap == 0 ? std::size_t{1} << 63 : 0) | std::size_t{Rewritten(slot)} << 55 |
                    std::size_t{cold_since[slot]} << RecoverSlotBits | slot;
        }
        std::sort(recover_order.begin(), recover_order.end());
        for (std::size_t& entry : recover_order) entry &= (std::size_t{1} << RecoverSlotBits) - 1;
        // More of them for the requests that follow: every slot unwritten for a while is probed now
        // (once a second at most), not at the scanner's next pass.
        static std::uint32_t probed_at = 0;
        if (probed_at != now) {
            probed_at = now;
            std::size_t made = 0;
            for (std::size_t slot = 0; slot < cold_since.size() && made < 4 * IdlePerPass; ++slot)
                made += ProbeSlot(*range, slot, now);
        }
    } else {
        // Without Idle slots (EDEN_CRAM_IDLE=0), as before them: Own slots unwritten for 10 s.
        for (std::size_t slot = 0; slot < cold_since.size(); ++slot)
            if (SlotState(*range, slot) == SlotOwn && now - cold_since[slot] >= 10) recover_order.push_back(slot);
        std::sort(recover_order.begin(), recover_order.end(), [](std::size_t a, std::size_t b) { return cold_since[a] < cold_since[b]; });
    }
    // A load can take the pool from comfortable to empty between two scanner passes: the swap store
    // is made ready here then (the driver's call: a thread that may allocate and print).
    if (driver && (wait || keeper) && idle_mode) (void)SwapStart();
    // Four steps, each only if the ones before did not give enough:
    //   0. What costs no write: slots that compress go to the packed store, and slots whose bytes
    //      the swap store holds (all of them: found by their contents) give their blocks up.
    //   1. Slots that were never written again after a while unwritten (swap_rewritten) go to the
    //      swap store with what it lacks of them written, the longest unwritten first.
    //   2. The blocks the service keeps idle go back (GiveBackIdleBlocks).
    //   3. For a request that would otherwise get nothing, the slots the game reuses go the same
    //      way: the budget is what limits the writes, and inside it an allocation that fails ends
    //      the session.
    // The keeper (no allocation failed yet) stops after step 0, or after step 1 when it may write.
    std::size_t freed = 0, given = 0, to_file = 0, last_resort = 0;
    const std::uint64_t written_before = swap_written.load(std::memory_order_relaxed);
    for (int step = 0; step < 4 && freed + given < bytes; ++step) {
        if (step == 2) {
            if (keeper) break;
            given = GiveBackIdleBlocks(bytes - freed, !driver);
            continue;
        }
        if (step != 0 && (!idle_mode || keeper == 1 || swap_state.load(std::memory_order_acquire) == 2)) continue;
        static const bool last_resort_on = EnvOr("EDEN_SWAP_LAST_RESORT", 1) != 0;
        if (step == 3 && !last_resort_on) break;
        // (The keeper leaves the quarter of the burst that is kept for failed allocations, and writes
        // only slots unwritten for KeeperColdSeconds.)
        const Swap swap = step == 0 ? Swap::Reuse : keeper ? Swap::Yes : Swap::Urgent;
        PackedSlot* const entries = packed_slots.load(std::memory_order_acquire);
        recovering = true;
        swap_refused = false;
        for (const std::size_t slot : recover_order) {
            // (Enough, or the budget or the store's room has said no: the slots left would be hashed for nothing.)
            if (freed + given >= bytes || (step != 0 && swap_refused)) break;
            const bool in_file = entries[slot].swap != 0;
            if (step != 0 && !in_file) {
                if ((Rewritten(slot) != 0) != (step == 3)) continue;
                if (keeper && now - cold_since[slot] < KeeperColdSeconds) continue;
            }
            if (PackSlot(*range, slot, swap)) {
                if (pack_released) freed += SparseSlot;
                if (!in_file && entries[slot].swap != 0) {
                    ++to_file;
                    last_resort += step == 3;
                }
                cold_since[slot] = now;
                if (pack_times[slot] < 255) ++pack_times[slot];
            }
        }
        // Slots in units: their idle units go too, with what the store lacks of them written from
        // step 1 on.
        if (freed + given < bytes && step < 2 && !(step != 0 && swap_refused) && UnitsReady())
            freed += UnitSweep(*range, now, keeper ? IdleSeconds() : UrgentIdleSeconds(), false,
                               step == 0 ? SwapWrites::None : keeper ? SwapWrites::Budget : SwapWrites::Urgent,
                               bytes - freed - given);
        recovering = false;
    }
    if (keeper) {
        keeper_calls.fetch_add(1, std::memory_order_relaxed);
        keeper_bytes.fetch_add(freed, std::memory_order_relaxed);
        return trimmed + freed;
    }
    swap_last_resort.fetch_add(last_resort, std::memory_order_relaxed);
    const auto calls = recover_calls.fetch_add(1, std::memory_order_relaxed);
    recovered_bytes.fetch_add(freed, std::memory_order_relaxed);
    const bool fell_short = freed + given < bytes && recover_short.fetch_add(1, std::memory_order_relaxed) < 100;
    if (driver && (calls < 24 || fell_short || (to_file != 0 && calls < 400))) {
        char line[320];
        const int length = std::snprintf(line, sizeof(line),
            "EDEN_CRAM_PACK recover%s asked=%zuMiB pool_free=%zuMiB packed=%zuMiB idle_blocks=%zuMiB to_swap=%zuMiB "
            "last_resort=%zuMiB swap_written=%lluMiB candidates=%zu\n",
            wait ? "" : "_try", asked >> 20, pool_free >> 20, freed >> 20, given >> 20, to_file * (SparseSlot >> 20),
            last_resort * (SparseSlot >> 20),
            (unsigned long long)((swap_written.load(std::memory_order_relaxed) - written_before) >> 20), recover_order.size());
        if (length > 0) Report(line);
    }
    return trimmed + freed + given;
#else
    (void)bytes;
    (void)wait;
    return 0;
#endif
}
} // namespace

namespace {
// The pool is kept from running empty. Asked back only once an allocation had failed, memory came
// from the thread that draws: 30 requests in a row at a race start of Mario Kart World, each one
// packing slots while a frame waited. So five times a second the keeper's thread looks at what
// the pool has free in 2 MiB pieces, and under EDEN_CRAM_KEEP_FREE_MIB (512; 0: off) it has slots
// nothing has touched for IdleSeconds give their blocks up until 256 MiB more than that is free.
//
// It does that with what costs no write: slots that compress go to the packed store, and slots
// the swap store already has give their blocks up. It used to write slots to the store as well
// (once the pool had stayed under the mark for 3 s). That kept 512 MiB free at all times at the
// storage's cost: in a 30-minute Mario Kart World session the keeper had most of 3 GiB written,
// two thirds of it for slots the game wrote over within minutes, while requests after a failed
// allocation, the only ones that show memory is needed, came 42 times. Free memory also draws
// takers: the graphics caches keep more when the pool has more. EDEN_SWAP_KEEPER=1 lets it
// write again (slots unwritten for KeeperColdSeconds and never written again after a while).
// What the pool had free in 2 MiB pieces when the keeper last counted them (it does while the
// largest free block is under its mark), for eden_pool_free_pieces.
std::atomic<std::size_t> pool_free_seen{0};
std::atomic<std::uint64_t> pool_free_seen_ns{0};
std::size_t KeepFreeMark() noexcept {
    static const std::size_t bytes = EnvOr("EDEN_CRAM_KEEP_FREE_MIB", 512) << 20;
    return bytes;
}
void KeepFree() noexcept {
#ifdef PS5_NATIVE
    const std::size_t mark = KeepFreeMark();
    if (mark == 0 || service_range.load(std::memory_order_acquire) == nullptr || !PackEnabled() ||
        packed_slots.load(std::memory_order_acquire) == nullptr)
        return;
    // One call first: a pool whose largest free block is that large has that much free.
    const auto total = sceKernelGetDirectMemorySize();
    std::int64_t start = 0;
    std::size_t largest = 0;
    if (total <= 0 || &sceKernelAvailableDirectMemorySize == nullptr ||
        sceKernelAvailableDirectMemorySize(0, total, SparseSlot, &start, &largest) != 0 || largest >= mark)
        return;
    const std::size_t free_pieces = PoolFreePieces();
    pool_free_seen.store(free_pieces, std::memory_order_relaxed);
    pool_free_seen_ns.store(SwapNow(), std::memory_order_release);
    static std::uint64_t below_since = 0; // the keeper's thread only
    if (free_pieces >= mark) {
        below_since = 0;
        return;
    }
    // Packing in memory at once. To the swap store, when it may at all (EDEN_SWAP_KEEPER=1), only
    // when the pool has stayed under the mark for 3 s or has a quarter of it left: a race start
    // takes 2 GB and gives half of it back within seconds.
    static const bool may_write = EnvOr("EDEN_SWAP_KEEPER", 0) != 0;
    const std::uint64_t now = SwapNow();
    if (below_since == 0) below_since = now;
    const bool write = may_write && (now - below_since >= 3'000'000'000ull || free_pieces < mark / 4);
    (void)RecoverMemory(mark + (std::size_t{256} << 20) - free_pieces, false, true, write ? 2 : 1);
#endif
}
} // namespace

// Waits for the scanner if it is packing (the driver's allocations).
extern "C" std::size_t eden_memory_recover(std::size_t bytes) noexcept { return RecoverMemory(bytes, true); }
// Never waits: for the heap, which may hold its growth lock while the scanner allocates.
extern "C" std::size_t eden_memory_recover_try(std::size_t bytes) noexcept { return RecoverMemory(bytes, false); }
// What the pool has free in 2 MiB pieces, as the keeper counted it within the last half second; 0
// when it has not (the largest free block is then above the keeper's mark, and says enough).
// headless/performance.cpp tells the graphics caches: the driver takes memory in such pieces
// (tools/patch-radv-pieces.py), so the largest free block alone understates what it can have. After
// PS5_Lichii's driver, whose budget and available memory "count the whole shared pool, in 2 MiB
// pieces" (docs/ps5/levers.md there).
extern "C" std::size_t eden_pool_free_pieces(void) noexcept {
#ifdef PS5_NATIVE
    const std::uint64_t seen = pool_free_seen_ns.load(std::memory_order_acquire);
    if (seen == 0 || SwapNow() - seen > 500'000'000ull) return 0;
    return pool_free_seen.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}
// The swap store as numbers, for the crash report (a signal handler: plain reads, no lock): MiB it
// holds, MiB it may hold, MiB written this session, slots the budget refused, slots its size
// refused. Returns 0 while it is not opened, 1 when it is on, 2 when it is off.
extern "C" int eden_swap_numbers(unsigned long long numbers[5]) noexcept {
    numbers[0] = std::uint64_t{swap_count} * SwapUnit >> 20;
    numbers[1] = std::uint64_t{swap_limit} * SwapUnit >> 20;
    numbers[2] = swap_written.load(std::memory_order_relaxed) >> 20;
    numbers[3] = swap_denied_rate.load(std::memory_order_relaxed);
    numbers[4] = swap_full.load(std::memory_order_relaxed);
    return swap_state.load(std::memory_order_relaxed);
}


// ---- What the guest's kernel tells the service ----

// Development switches (dev-settings cram=off, cram_scan=off, cram_dedup=off), before the guest's
// memory is taken.
void SetGuestMemoryService(bool on, bool scan, bool dedup) noexcept {
    service_requested.store(on, std::memory_order_relaxed);
    scan_requested.store(scan, std::memory_order_relaxed);
    dedup_requested.store(dedup, std::memory_order_relaxed);
}

// Settings > Performance > Compact memory (off unless chosen): written slots with many pages of
// zeroes keep only the others. It gives back more memory, and a game that writes such a slot again
// waits for it to be made whole (a fault and a 2 MiB copy, about 0.3 ms).
void SetGuestMemoryFine(bool on) noexcept {
    fine_requested.store(on, std::memory_order_relaxed);
}

// The guest's kernel hands pages out: they are committed now only without the service.
void CommitGuestMemoryOnHandOut(std::uint64_t offset, std::size_t bytes) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range != nullptr && range->promote) return;
    CommitGuestMemory(offset, bytes);
}

// The guest's kernel fills pages (`at` is the backing at `offset`). With the service, a fill with
// zeroes leaves Zero slots alone and gives whole Own or Shared slots back; anything else is written.
void ClearGuestMemory(std::uint8_t* at, std::uint64_t offset, std::size_t bytes, int fill) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range == nullptr || !range->promote || fill != 0) {
        if (range != nullptr && !range->promote) CommitGuestMemory(offset, bytes);
        std::memset(at, fill, bytes); // (with the service, a write to a Zero slot promotes it)
        return;
    }
    const std::uint64_t end = offset + bytes;
    for (std::uint64_t cursor = offset; cursor < end;) {
        const std::size_t slot = static_cast<std::size_t>(cursor / SparseSlot);
        const std::uint64_t slot_end = (cursor / SparseSlot + 1) * SparseSlot;
        const std::uint64_t piece_end = std::min(end, slot_end);
        const bool whole = cursor % SparseSlot == 0 && piece_end == slot_end;
        if (whole) {
            LockSlot(*range, slot);
            const auto state = SlotState(*range, slot);
            if (state != SlotZero) {
                ReturnSlot(*range, slot);
                counters.returns_clear.fetch_add(1, std::memory_order_relaxed);
            }
            UnlockSlot(*range, slot, SlotZero);
        } else if (SlotState(*range, slot) != SlotZero) {
            std::memset(at + (cursor - offset), 0, static_cast<std::size_t>(piece_end - cursor));
        }
        cursor = piece_end;
    }
}

// A page the guest's kernel fills one at a time (optimized allocation): the same rule.
void FillGuestMemory(std::uint8_t* at, int fill, std::size_t bytes) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range != nullptr && range->promote && fill == 0) {
        const auto begin = range->begin.load(std::memory_order_relaxed);
        const auto address = reinterpret_cast<std::uintptr_t>(at);
        if (address >= begin && address < range->end && SlotState(*range, (address - begin) / SparseSlot) == SlotZero)
            return;
    }
    std::memset(at, fill, bytes);
}

// The guest's kernel freed pages (a free block of 2 MiB or more in its page heap): every slot they
// cover whole goes back. EDEN_GUEST_FREE_RETURN=0 keeps freed slots held until they are reused.
bool ReturnFreedSlots() noexcept {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_GUEST_FREE_RETURN");
        return value == nullptr || value[0] != '0';
    }();
    return on;
}

void GuestMemoryFreed(std::uint64_t offset, std::size_t bytes) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range == nullptr || !range->promote || bytes < SparseSlot || !ReturnFreedSlots()) return;
    const std::uint64_t first = (offset + SparseSlot - 1) / SparseSlot;
    const std::uint64_t last = (offset + bytes) / SparseSlot; // exclusive
    for (std::uint64_t slot = first; slot < last; ++slot) {
        LockSlot(*range, static_cast<std::size_t>(slot));
        if (SlotState(*range, static_cast<std::size_t>(slot)) != SlotZero) {
            ReturnSlot(*range, static_cast<std::size_t>(slot));
            counters.returns_free.fetch_add(1, std::memory_order_relaxed);
        }
        UnlockSlot(*range, static_cast<std::size_t>(slot), SlotZero);
    }
}

// The guest's kernel freed pages that do not make a free block of 2 MiB (a region's tail, a page
// group's blocks): GuestMemoryFreed never sees them, and the slot they sit in stays held whole.
// MKW unmaps ~1.7 GB in a 5-minute session in pieces of ~370 KiB, and ~640 MiB of it was never
// given back. Nothing in the guest can read freed pages (the kernel fills or hands them out again
// before use), so they are cleared where the slot has a block of its own; the scanner then gives
// back the slot (all zero) or its pages of zeroes (Fine). EDEN_GUEST_FREE_ZERO=0 leaves them.
bool ZeroFreedParts() noexcept {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_GUEST_FREE_ZERO");
        return value == nullptr || value[0] != '0';
    }();
    return on;
}

void GuestMemoryFreedPart(std::uint64_t offset, std::size_t bytes) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range == nullptr || !range->promote || bytes == 0 || !ZeroFreedParts()) return;
    const std::size_t slots = (range->end - range->begin.load(std::memory_order_relaxed)) / SparseSlot;
    freed_part_calls.fetch_add(1, std::memory_order_relaxed);
    for (std::uint64_t at = offset, end = offset + bytes; at < end;) {
        const std::size_t slot = static_cast<std::size_t>(at / SparseSlot);
        const std::uint64_t slot_end = std::min<std::uint64_t>(end, (at / SparseSlot + 1) * SparseSlot);
        if (slot >= slots) break;
        LockSlot(*range, slot);
        const std::uint8_t state = SlotState(*range, slot);
        if (state == SlotOwn) {
            std::memset(reinterpret_cast<void*>(SlotAddress(*range, slot) + at % SparseSlot), 0,
                        static_cast<std::size_t>(slot_end - at));
            freed_part_bytes.fetch_add(slot_end - at, std::memory_order_relaxed);
        }
        UnlockSlot(*range, slot, state);
        at = slot_end;
    }
}

// Host memory behind [offset, offset + bytes) of the guest's memory (the memory report splits it
// by the guest kernel's pools). Whole slots count 2 MiB, Fine slots their kept pages.
std::size_t GuestMemoryHeldIn(std::uint64_t offset, std::size_t bytes) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range == nullptr || range->state == nullptr || bytes == 0) return 0;
    const std::size_t slots = (range->end - range->begin.load(std::memory_order_relaxed)) / SparseSlot;
    std::size_t held = 0;
    const std::size_t last = std::min<std::size_t>(slots, (offset + bytes + SparseSlot - 1) / SparseSlot);
    for (std::size_t slot = static_cast<std::size_t>(offset / SparseSlot); slot < last; ++slot) {
        const auto state = SlotState(*range, slot);
        if (state == SlotOwn || state == SlotShared || state == SlotIdle || state == SlotClean) held += SparseSlot;
        else if (state == SlotFine) held += FineKept(*range, slot) * FinePage;
    }
    return held;
}

// For the host checks: one scan pass now, and a slot's state (0 Zero, 1 Own, 2 Shared, -1 none).
void GuestMemoryScanNow() noexcept {
    if (SparseRange* const range = service_range.load(std::memory_order_acquire)) ScanPass(*range);
}
int GuestMemorySlotState(std::uint64_t offset) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range == nullptr || range->state == nullptr) return -1;
    return SlotState(*range, static_cast<std::size_t>(offset / SparseSlot));
}

// ---- The 64-bit fastmem window (src/host_memory.cpp, Window64; backported from ProsperoEden) ----

// From a JIT fault in the window: shows the slot at `backing` (2 MiB aligned) at `window_at`. An Own
// slot shows its block, read-write when `writable`; a Zero or Shared slot shows its block read-only
// for a read. A write to a slot without a block of its own sets `promote` and shows nothing: the
// caller promotes it as a write to the backing would and asks again. False otherwise (the access
// then takes the page-table path).
bool AliasGuestSlot(std::uintptr_t backing, std::uintptr_t window_at, bool writable, bool write, bool* promote) noexcept {
#ifdef PS5_NATIVE
    auto* range = guest_range.load(std::memory_order_acquire);
    if (!range || !range->state || !range->aliases) return false;
    if (!window_seen.load(std::memory_order_relaxed)) window_seen.store(true, std::memory_order_relaxed);
    const std::uintptr_t begin = range->begin.load(std::memory_order_relaxed);
    if (backing < begin || backing >= range->end || (backing - begin) % SparseSlot || window_at % SparseSlot) return false;
    const std::size_t slot = (backing - begin) / SparseSlot;
    LockSlot(*range, slot);
    const std::uint8_t state = SlotState(*range, slot);
    std::int64_t physical = -1;
    if (state == SlotOwn) {
        physical = range->physical[slot];
    } else if (write || state == SlotPacked || state == SlotIdle) {
        *promote = true; // (a Packed or Idle slot is woken by any access, as through the backing)
    } else if (state == SlotZero) {
        physical = zero_block, writable = false;
    } else if (state == SlotShared || state == SlotClean) {
        physical = range->physical[slot], writable = false;
    }
    bool shown = false;
    if (physical >= 0) {
        const int protection = writable ? PROT_READ | PROT_WRITE : PROT_READ;
        const std::uintptr_t entry = window_at | (writable ? 0 : AliasReadOnly);
        std::uintptr_t* free_entry = nullptr;
        bool known = false;
        for (unsigned i = 0; i < AliasesPerSlot; ++i) {
            auto& alias = range->aliases[slot * AliasesPerSlot + i];
            if ((alias & ~AliasFlags) == window_at) {
                // Another core showed it first, or it shows it read-only or in pieces.
                known = true;
                shown = alias == entry || (MapSlot(window_at, protection, physical) && ((alias = entry), true));
                break;
            }
            if (!alias && !free_entry) free_entry = &alias;
        }
        if (!known && free_entry && MapSlot(window_at, protection, physical)) {
            *free_entry = entry;
            shown = true;
            aliases_made.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!shown) aliases_refused.fetch_add(1, std::memory_order_relaxed);
    UnlockSlot(*range, slot, state);
    return shown;
#else
    (void)backing, (void)window_at, (void)writable, (void)write, (void)promote;
    return false; // the host's guest memory is private: nothing to alias
#endif
}

// From a JIT fault in a window region that cannot show a whole block (not on a 2 MiB boundary of the
// backing, or partly protected): shows the 16 KiB at `backing` of an Own slot at `at`, inside the
// window region `region_at`. The slot's entry names the whole region, so any change of the slot voids
// it. A write to a slot that is not Own sets `promote`, as for AliasGuestSlot.
bool AliasGuestPage(std::uintptr_t backing, std::uintptr_t at, std::uintptr_t region_at, bool writable, bool write,
                    bool* promote) noexcept {
#ifdef PS5_NATIVE
    auto* range = guest_range.load(std::memory_order_acquire);
    if (!range || !range->state || !range->aliases) return false;
    if (!window_seen.load(std::memory_order_relaxed)) window_seen.store(true, std::memory_order_relaxed);
    const std::uintptr_t begin = range->begin.load(std::memory_order_relaxed);
    if (backing < begin || backing >= range->end || (backing - begin) % FinePage || at % FinePage) return false;
    const std::size_t slot = (backing - begin) / SparseSlot;
    LockSlot(*range, slot);
    const std::uint8_t state = SlotState(*range, slot);
    bool shown = false;
    // A Zero slot shows the zero block and a Shared one its shared block, read-only; a write there
    // promotes the slot first (the caller). A Fine slot keeps its pages apart: not shown.
    const std::size_t offset = backing - SlotAddress(*range, slot);
    std::int64_t physical = -1;
    if (state == SlotOwn || (!write && (state == SlotShared || state == SlotClean))) {
        physical = range->physical[slot] >= 0 ? range->physical[slot] + std::int64_t(offset) : -1;
    } else if (!write && state == SlotZero) {
        physical = zero_block + std::int64_t(offset);
    } else if (write || state == SlotPacked || state == SlotIdle) {
        *promote = true;
    }
    if (state != SlotOwn) writable = false;
    if (physical >= 0) {
        std::uintptr_t* entry = nullptr;
        bool known = false;
        for (unsigned i = 0; i < AliasesPerSlot && !known; ++i) {
            auto& alias = range->aliases[slot * AliasesPerSlot + i];
            if ((alias & ~AliasFlags) == region_at) entry = &alias, known = true;
            else if (!alias && !entry) entry = &alias;
        }
        void* address = reinterpret_cast<void*>(at);
        if (entry && sceKernelMapDirectMemory(&address, FinePage, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_FIXED,
                                              physical, FinePage) == 0 && address == reinterpret_cast<void*>(at)) {
            if (!known) aliases_made.fetch_add(1, std::memory_order_relaxed);
            *entry = region_at | AliasSmall;
            shown = true;
        }
    }
    if (!shown) aliases_refused.fetch_add(1, std::memory_order_relaxed);
    UnlockSlot(*range, slot, state);
    return shown;
#else
    (void)backing, (void)at, (void)region_at, (void)writable, (void)write, (void)promote;
    return false;
#endif
}

// The window region at `window_at` no longer shows the slot at `backing` (guest unmap, protection).
// True when it did.
bool DropGuestAlias(std::uintptr_t backing, std::uintptr_t window_at) noexcept {
    auto* range = guest_range.load(std::memory_order_acquire);
    if (!range || !range->state || !range->aliases) return false;
    if (!window_seen.load(std::memory_order_relaxed)) window_seen.store(true, std::memory_order_relaxed);
    const std::uintptr_t begin = range->begin.load(std::memory_order_relaxed);
    if (backing < begin || backing >= range->end) return false;
    const std::size_t slot = (backing - begin) / SparseSlot;
    bool dropped = false;
    LockSlot(*range, slot);
    for (unsigned i = 0; i < AliasesPerSlot; ++i)
        if (auto& alias = range->aliases[slot * AliasesPerSlot + i]; alias && (alias & ~AliasFlags) == window_at) {
            VoidWindowRegion(window_at);
            alias = 0;
            dropped = true;
            aliases_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    UnlockSlot(*range, slot, SlotState(*range, slot));
    return dropped;
}

void GuestAliasCounts(std::uint64_t* made, std::uint64_t* dropped, std::uint64_t* refused) noexcept {
    *made = aliases_made.load(std::memory_order_relaxed);
    *dropped = aliases_dropped.load(std::memory_order_relaxed);
    *refused = aliases_refused.load(std::memory_order_relaxed);
}
