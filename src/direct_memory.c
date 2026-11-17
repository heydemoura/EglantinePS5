// SPDX-License-Identifier: GPL-3.0-or-later
// Where the app's long-lived memory goes in the pool the CPU shares with the GPU.
//
// The system gives out direct memory from the bottom of the pool: the lowest room that fits.
// A game's own memory (4 GiB) and its graphics come and go with each session, while two kinds
// of blocks stay for good: the pieces the heap grows by (headless/heap_arenas.inc) and the
// stacks of threads (the Payload SDK takes 2 MiB of direct memory for each, and keeps freed
// ones for the next threads). Taken from the bottom like the rest, those landed between a
// running game's graphics memory, and when the game ended they were left spread over the
// middle of the pool. The next game's graphics get the largest free stretch, which was then
// much smaller than what was free in total, and smaller again after every game.
//
// So blocks that stay are taken from the top of the pool downwards, and everything a session
// takes and gives back is left to the system as before. The link wraps the calls
// (tools/link-headless-native.sh): the heap asks for the top itself (eden_direct_high), and
// the other blocks that stay are known by their size: a thread's stack is 2 MiB, and so is the
// one block the graphics driver keeps for good (its zero block, taken in the middle of a game
// and then left where it was). The driver's command ring and marker page are known by who
// asks. The app's own 2 MiB blocks, the slots of the guest page tables, go back when a game
// ends: they ask below the wrap (eden_direct_low) and stay where the system puts them.
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int __real_sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
int __real_sceKernelReleaseDirectMemory(int64_t, size_t);
int64_t sceKernelGetDirectMemorySize(void);

#define EDEN_DIRECT_STEP ((int64_t)2 << 20)       /* blocks at the top are placed in 2 MiB steps */
#define EDEN_DIRECT_SMALL ((size_t)2 << 20)       /* a thread's stack; the driver's zero block */

static atomic_flag eden_direct_lock = ATOMIC_FLAG_INIT;
static int64_t eden_direct_floor;                 /* the lowest block taken at the top; 0: none yet */
static int64_t eden_direct_holes[1024];           /* 2 MiB blocks given back above the floor */
static unsigned eden_direct_hole_count;

static void eden_direct_acquire(void) {
  while (atomic_flag_test_and_set_explicit(&eden_direct_lock, memory_order_acquire)) {}
}
static void eden_direct_release(void) { atomic_flag_clear_explicit(&eden_direct_lock, memory_order_release); }

/* Memory from as high in the pool as there is room; nonzero when the top has none. */
static int eden_direct_high_only(size_t size, size_t alignment, int type, int64_t *start) {
  const int64_t total = sceKernelGetDirectMemorySize();
  const int64_t bytes = (int64_t)size;
  if (bytes <= 0 || total <= 0) return -1;
  eden_direct_acquire();
  /* A block given back earlier left a hole of its size. */
  while (size == EDEN_DIRECT_SMALL && eden_direct_hole_count != 0) {
    const int64_t hole = eden_direct_holes[--eden_direct_hole_count];
    if (__real_sceKernelAllocateDirectMemory(hole, hole + bytes, size, alignment, type, start) == 0) {
      eden_direct_release();
      return 0;
    }
  }
  /* Below what is at the top already; past what something else has put there, for a while. */
  int64_t next = eden_direct_floor != 0 ? eden_direct_floor : total;
  for (unsigned tries = 0; tries < 512 && next >= bytes; ++tries) {
    const int64_t at = (next - bytes) / EDEN_DIRECT_STEP * EDEN_DIRECT_STEP;
    next = at;
    if (__real_sceKernelAllocateDirectMemory(at, at + bytes, size, alignment, type, start) == 0) {
      eden_direct_floor = at;
      eden_direct_release();
      return 0;
    }
  }
  eden_direct_release();
  return -1;
}

/* Memory from as high in the pool as there is room; from anywhere when the top has none. */
int eden_direct_high(size_t size, size_t alignment, int type, int64_t *start) {
  if (eden_direct_high_only(size, alignment, type, start) == 0) return 0;
  return __real_sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, alignment, type, start);
}

/* As the system places it: for what a session takes and gives back. */
int eden_direct_low(size_t size, size_t alignment, int type, int64_t *start) {
  return __real_sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, alignment, type, start);
}

/* The guest's memory in runs (EDEN_GUEST_RUNS=1).
 *
 * The guest memory service (src/guest_memory_service.inc) takes and gives back 2 MiB blocks one at
 * a time, by the thousand, for the whole session. Taken from the bottom like everything else, they
 * sat between the driver's blocks (16-64 MiB) and the heap's pieces: Mario Kart World raced with
 * 0.7-0.85 GB free in ~900 holes and a largest free block of 14-30 MiB, and what packing gave back
 * was more 2 MiB holes there. So with the switch on the guest's blocks go to a zone at the top of
 * the pool, with the blocks that stay (eden_direct_high), and the driver keeps the bottom; the free
 * memory between the two is one run.
 *
 *   - The zone is [eden_guest_lo, top). It grows downwards by runs of 64 MiB, taken where
 *     eden_direct_high would put them (just below what is at the top already).
 *   - 2 MiB pieces of the zone that are known to be free (given back by anyone) are in a bitmap; a
 *     block is taken from the highest of them first, so the bottom of the zone, next to the free
 *     run, is the part that empties when the guest gives memory back.
 *   - Smaller blocks (the compact blocks of Fine slots) are cut from one 2 MiB piece of the zone at
 *     a time, not from the bottom of the pool.
 *   - When the zone has no room and cannot grow, a block comes from anywhere, as before.
 *
 * Only where a block comes from changes: the service takes and gives back the same blocks, mapped
 * the same way, with its slot states as they were. */
#define EDEN_GUEST_PIECE ((int64_t)2 << 20)
#define EDEN_GUEST_GROW ((size_t)64 << 20)
#define EDEN_GUEST_WORDS 128 /* 2 MiB pieces of up to 16 GiB */
static atomic_int eden_guest_runs = -1;            /* -1 not read yet */
static _Atomic int64_t eden_guest_lo;                  /* 0: no zone yet */
static atomic_ullong eden_guest_known[EDEN_GUEST_WORDS];
static atomic_flag eden_guest_grow_lock = ATOMIC_FLAG_INIT;
static atomic_uint eden_guest_grow_wait;           /* slow paths to skip growing after it failed */
static _Atomic int64_t eden_guest_small = -1;          /* the piece small blocks are cut from */
static atomic_ullong eden_guest_hits, eden_guest_misses, eden_guest_zone_fits, eden_guest_grows,
    eden_guest_grow_fails, eden_guest_fallbacks, eden_guest_small_hits, eden_guest_small_pieces;

int eden_direct_guest_runs(void) {
  int on = atomic_load_explicit(&eden_guest_runs, memory_order_relaxed);
  if (on < 0) {
    const char *value = getenv("EDEN_GUEST_RUNS");
    on = value != NULL && value[0] == '1';
    atomic_store_explicit(&eden_guest_runs, on, memory_order_relaxed);
  }
  return on;
}
/* main reads env.txt after the first blocks were taken: the switch is read again then. */
void eden_direct_guest_configure(void) { atomic_store_explicit(&eden_guest_runs, -1, memory_order_relaxed); }
int64_t eden_direct_guest_lo(void) {
  return eden_direct_guest_runs() ? atomic_load_explicit(&eden_guest_lo, memory_order_relaxed) : 0;
}

/* Pieces [from, to) of the zone are free. */
static void eden_guest_mark_free(int64_t from, int64_t to) {
  const int64_t lo = atomic_load_explicit(&eden_guest_lo, memory_order_relaxed);
  if (lo == 0 || to <= lo) return;
  if (from < lo) from = lo;
  int64_t piece = (from + EDEN_GUEST_PIECE - 1) / EDEN_GUEST_PIECE;
  const int64_t end = to / EDEN_GUEST_PIECE;
  for (; piece < end && piece < (int64_t)EDEN_GUEST_WORDS * 64; ++piece)
    atomic_fetch_or_explicit(&eden_guest_known[piece / 64], 1ULL << (piece % 64), memory_order_relaxed);
}

/* The highest piece known to be free, taken off the bitmap; -1 none. */
static int64_t eden_guest_claim(void) {
  for (int word = EDEN_GUEST_WORDS - 1; word >= 0; --word) {
    unsigned long long bits = atomic_load_explicit(&eden_guest_known[word], memory_order_relaxed);
    while (bits != 0) {
      const int bit = 63 - __builtin_clzll(bits);
      if (atomic_compare_exchange_weak_explicit(&eden_guest_known[word], &bits, bits & ~(1ULL << bit),
                                                memory_order_relaxed, memory_order_relaxed))
        return ((int64_t)word * 64 + bit) * EDEN_GUEST_PIECE;
    }
  }
  return -1;
}

/* The zone a run of 64 MiB lower; false when the top has no such run (then not tried again for a while). */
static int eden_guest_grow(int type) {
  unsigned wait = atomic_load_explicit(&eden_guest_grow_wait, memory_order_relaxed);
  while (wait != 0 && !atomic_compare_exchange_weak_explicit(&eden_guest_grow_wait, &wait, wait - 1,
                                                             memory_order_relaxed, memory_order_relaxed)) {}
  if (wait != 0) return 0;
  while (atomic_flag_test_and_set_explicit(&eden_guest_grow_lock, memory_order_acquire)) {}
  int64_t run = -1;
  const int grown = eden_direct_high_only(EDEN_GUEST_GROW, (size_t)EDEN_GUEST_PIECE, type, &run) == 0;
  if (grown) {
    const int64_t lo = atomic_load_explicit(&eden_guest_lo, memory_order_relaxed);
    if (lo == 0 || run < lo) atomic_store_explicit(&eden_guest_lo, run, memory_order_relaxed);
    /* Given back at once: the zone holds nothing it does not use. The next block takes from it. */
    (void)__real_sceKernelReleaseDirectMemory(run, EDEN_GUEST_GROW);
    eden_guest_mark_free(run, run + (int64_t)EDEN_GUEST_GROW);
    atomic_fetch_add_explicit(&eden_guest_grows, 1, memory_order_relaxed);
  } else {
    atomic_store_explicit(&eden_guest_grow_wait, 256, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_guest_grow_fails, 1, memory_order_relaxed);
  }
  atomic_flag_clear_explicit(&eden_guest_grow_lock, memory_order_release);
  return grown;
}

/* A block of the guest's memory (its 2 MiB slots and the blocks the service keeps for them). */
int eden_direct_guest(size_t size, size_t alignment, int type, int64_t *start) {
  if (!eden_direct_guest_runs() || start == NULL || size == 0 || size > (size_t)EDEN_GUEST_PIECE)
    return eden_direct_low(size, alignment, type, start);
  const int64_t total = sceKernelGetDirectMemorySize();
  for (int round = 0; round < 2; ++round) {
    if (size < (size_t)EDEN_GUEST_PIECE) {
      /* Cut from the current small piece, else from a new one. */
      for (int tries = 0; tries < 4; ++tries) {
        int64_t piece = atomic_load_explicit(&eden_guest_small, memory_order_relaxed);
        if (piece >= 0 &&
            __real_sceKernelAllocateDirectMemory(piece, piece + EDEN_GUEST_PIECE, size, alignment, type, start) == 0) {
          atomic_fetch_add_explicit(&eden_guest_small_hits, 1, memory_order_relaxed);
          return 0;
        }
        const int64_t fresh = eden_guest_claim();
        if (fresh < 0) break;
        atomic_fetch_add_explicit(&eden_guest_small_pieces, 1, memory_order_relaxed);
        atomic_compare_exchange_strong_explicit(&eden_guest_small, &piece, fresh, memory_order_relaxed,
                                                memory_order_relaxed);
      }
    } else {
      /* The highest free piece of the zone. */
      for (int tries = 0; tries < 64; ++tries) {
        const int64_t piece = eden_guest_claim();
        if (piece < 0) break;
        if (__real_sceKernelAllocateDirectMemory(piece, piece + EDEN_GUEST_PIECE, size, alignment, type, start) == 0) {
          atomic_fetch_add_explicit(&eden_guest_hits, 1, memory_order_relaxed);
          return 0;
        }
        atomic_fetch_add_explicit(&eden_guest_misses, 1, memory_order_relaxed);
      }
    }
    /* Anywhere in the zone; the bitmap does not know every free piece. */
    const int64_t lo = atomic_load_explicit(&eden_guest_lo, memory_order_relaxed);
    if (lo != 0 && __real_sceKernelAllocateDirectMemory(lo, total, size, alignment, type, start) == 0) {
      atomic_fetch_add_explicit(&eden_guest_zone_fits, 1, memory_order_relaxed);
      return 0;
    }
    if (round == 0 && !eden_guest_grow(type)) break;
  }
  atomic_fetch_add_explicit(&eden_guest_fallbacks, 1, memory_order_relaxed);
  return eden_direct_low(size, alignment, type, start);
}

/* One line for stderr.log (headless/performance.cpp). */
int eden_direct_guest_report(char *out, size_t bytes) {
  if (!eden_direct_guest_runs()) return 0;
  unsigned long long known = 0;
  for (int word = 0; word < EDEN_GUEST_WORDS; ++word)
    known += (unsigned long long)__builtin_popcountll(atomic_load_explicit(&eden_guest_known[word], memory_order_relaxed));
  return snprintf(out, bytes,
                  "EDEN_GUEST_RUNS zone_lo=%lldMiB floor=%lldMiB known_free=%lluMiB hits=%llu misses=%llu zone_fits=%llu"
                  " grows=%llu grow_fails=%llu fallbacks=%llu small=%llu small_pieces=%llu\n",
                  (long long)(atomic_load_explicit(&eden_guest_lo, memory_order_relaxed) >> 20),
                  (long long)(eden_direct_floor >> 20), known * 2,
                  (unsigned long long)atomic_load(&eden_guest_hits), (unsigned long long)atomic_load(&eden_guest_misses),
                  (unsigned long long)atomic_load(&eden_guest_zone_fits), (unsigned long long)atomic_load(&eden_guest_grows),
                  (unsigned long long)atomic_load(&eden_guest_grow_fails),
                  (unsigned long long)atomic_load(&eden_guest_fallbacks),
                  (unsigned long long)atomic_load(&eden_guest_small_hits),
                  (unsigned long long)atomic_load(&eden_guest_small_pieces));
}

/* The Vulkan driver makes its command ring (16 MiB) and, right after it, the ring's marker page
 * once, with the first device, and keeps them until the app ends. Taken from the bottom like a
 * session's memory they landed above the first game's (the ring at 4.6 GiB), and every later
 * game found the pool's largest free stretch cut from 10.3 GiB to 5.7. They are known by who
 * asks (the driver's allocator; no such symbol in a build without that driver), by their sizes
 * and by being the first of those sizes in that order. */
extern void eden_radv_private_radv_ps5_memory_alloc(void) __attribute__((weak));
#define EDEN_DIRECT_RING ((size_t)16 << 20)
#define EDEN_DIRECT_PAGE ((size_t)16 << 10)
static atomic_int eden_direct_driver_kept; /* 0: neither yet; 1: the ring; 2: the marker page too */

static int eden_direct_driver_keeps(size_t size, const void *caller) {
  const char *const allocator = (const char *)(uintptr_t)&eden_radv_private_radv_ps5_memory_alloc;
  if (!allocator || (const char *)caller < allocator || (const char *)caller >= allocator + 2048) return 0;
  int kept = atomic_load_explicit(&eden_direct_driver_kept, memory_order_relaxed);
  if (kept == 0 && size == EDEN_DIRECT_RING)
    return atomic_compare_exchange_strong(&eden_direct_driver_kept, &kept, 1);
  if (kept == 1 && size == EDEN_DIRECT_PAGE)
    return atomic_compare_exchange_strong(&eden_direct_driver_kept, &kept, 2);
  return 0;
}

int __wrap_sceKernelAllocateDirectMemory(int64_t from, int64_t to, size_t size, size_t alignment, int type,
                                         int64_t *start) {
  /* A thread's stack or the driver's zero block, asked for anywhere in the pool; and the
   * driver's command ring and marker page. */
  if (from == 0 && start && to >= sceKernelGetDirectMemorySize() &&
      (size == EDEN_DIRECT_SMALL || eden_direct_driver_keeps(size, __builtin_return_address(0))))
    return eden_direct_high(size, alignment, type, start);
  return __real_sceKernelAllocateDirectMemory(from, to, size, alignment, type, start);
}

int __wrap_sceKernelReleaseDirectMemory(int64_t start, size_t size) {
  const int result = __real_sceKernelReleaseDirectMemory(start, size);
  if (result != 0) return result;
  if (atomic_load_explicit(&eden_guest_runs, memory_order_relaxed) == 1) {
    eden_guest_mark_free(start, start + (int64_t)size);
    /* A small block that was the last in its piece leaves the whole piece free: known again. */
    const int64_t piece = start / EDEN_GUEST_PIECE * EDEN_GUEST_PIECE;
    const int64_t lo = atomic_load_explicit(&eden_guest_lo, memory_order_relaxed);
    int64_t whole = -1;
    if (size < (size_t)EDEN_GUEST_PIECE && lo != 0 && piece >= lo &&
        piece != atomic_load_explicit(&eden_guest_small, memory_order_relaxed) &&
        __real_sceKernelAllocateDirectMemory(piece, piece + EDEN_GUEST_PIECE, (size_t)EDEN_GUEST_PIECE,
                                             (size_t)EDEN_GUEST_PIECE, 12, &whole) == 0) {
      (void)__real_sceKernelReleaseDirectMemory(whole, (size_t)EDEN_GUEST_PIECE);
      eden_guest_mark_free(piece, piece + EDEN_GUEST_PIECE);
    }
  }
  eden_direct_acquire();
  if (size == EDEN_DIRECT_SMALL && eden_direct_floor != 0 && start >= eden_direct_floor &&
      eden_direct_hole_count < sizeof(eden_direct_holes) / sizeof(eden_direct_holes[0]))
    eden_direct_holes[eden_direct_hole_count++] = start;
  eden_direct_release();
  return result;
}
