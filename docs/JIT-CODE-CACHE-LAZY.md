# JIT code caches: memory by the 2 MiB piece, 320 MiB a core

Status, 2026-11-23: written and checked on the host (mocked native calls); built for the console
and run there since in four sessions with the rest of the plan (`docs/LICHII-MEMORY-PLAN.md`, "The
console runs": Mario Kart World's six caches were 320 MiB each with 20 MiB held at the start),
without the checks listed at the end being made one by one. Step 4 of `docs/LICHII-MEMORY-PLAN.md`.

**On by default.** `EDEN_JIT_LAZY_CACHE=0` (`env.txt`) keeps the caches as they were.
`EDEN_PS5_CODE_CACHE_MIB=<n>` sets the size of every core's cache.

## What was there

The plan says the caches were committed at once. They were not: since the Mario Kart World
memory work (`docs/MKW-MEMORY-LEAK.md`, "JIT code cache under pressure") a cache of 32 MiB or
more is address space only, in two ranges, and takes memory as the JIT reaches it.

- `headless/jit-allocator.h`, `EdenJitAllocator`: xbyak's allocator. `alloc` asks
  `Common::ReserveCodeRange` for the two ranges and commits the first step; `commit` is
  `Common::CommitCodeRange`; without ranges the cache is taken whole (`AllocateMemoryPages` and
  `MapExecutableAlias`).
- Two views of the same memory, never one page that is written and run: the JIT writes through
  a read-write view and the code runs from a read-execute view (`sceKernelEnableDmemAliasing`,
  both views mapped with `sceKernelMapDirectMemory(MAP_FIXED)`, the second as read-only and then
  made executable with `mprotect`, because the mapping call refuses execute on some versions).
  With two views `BlockOfCode` changes no page protection per block (`hasWritableAlias()`).
  None of this changed.
- The JIT asks before it writes, never by a fault: `BlockOfCode::EnsureMemoryCommitted` calls
  `commitMemory(size_ + codesize)` before every block (1 MiB ahead: `a64_interface.cpp`
  `GetBlock` and `EdenPrecompile`, `a32_interface.cpp` `GetBasicBlock`), for the constant pool,
  and once for 16 MiB when the JIT is made (`PRELUDE_COMMIT_SIZE`).
- `src/memory_pages.cpp`, "A range that fills": memory came in steps of 16 MiB, each taken as
  eight 2 MiB pieces from the pool (`eden_direct_low`). A refused step asked the guest memory
  service for memory back up to three times, and ended the app when it gave none.

What that cost a Switch 2 title (six cores, 160/144/96/96/96/96 MiB): 32 MiB a cache when the
JITs are made (2 MiB of constants plus the 16 MiB prelude commit, rounded up to two steps),
192 MiB together, and afterwards up to 16 MiB more than each cache's code.

The sizes were small for another reason. The cores share their blocks (`jit_group.h`): a block
in one core's cache jumps to a block in another's with a 32-bit offset, and a link out of that
reach is left to the dispatcher. Each cache reserved its written view and then its executable
view where the system had room, so six caches spanned twice their sizes and had to stay under
1 GiB together. And one core's full cache clears all six.

## What PS5_Lichii does

PS5_Lichii (mihawk, `codeberg.org/mihawk/PS5_Lichii`, commit `5a424f7`):

- `src/core/arm/dynarmic/arm_dynarmic_64.cpp`: 320 MiB for every core, or
  `EDEN_PS5_CODE_CACHE_MIB` (64 to 1024). At 128 MiB Ocarina of Time's field filled a cache
  every 40 s (16.6 fps, 327 frames over 100 ms in 220 s); at 320 MiB it did not fill (21.9 fps,
  106). Its six caches must lie within 2 GiB of the title's own code, and 384 MiB each did not
  fit.
- `src/dynarmic/.../block_of_code.cpp`, `Ps5CodeAllocator`, and `src/common/host_memory.cpp`,
  `Ps5ReserveCodeOnTouch`: the cache is one range, readable, writable and executable from the
  start, with no memory behind it. A write to a 64 KiB unit without memory faults, and the
  fault handler backs the unit. Nothing is given back while the cache lives:
  `BlockOfCode::ClearCache` only moves the write position to the start.
- Its cores do not share blocks (`docs/ps5/jit-shared-cache.md`: tried, not taken), so each
  cache holds its core's copy of the game's code: 1.0 to 1.2 GB backed of 1.9 GB reserved.

What is not taken from it: the 64 KiB units (the pool here is shared with the driver in 2 MiB
pieces), backing by fault (the JIT here already asks before it writes) and one
read-write-execute range (the two views stay).

## What changed

`src/memory_pages.cpp`:

- **A step is one 2 MiB piece** (`CodePiece`, `CommitCodePiece`), not eight. A cache holds its
  code rounded up to 2 MiB: 20 MiB when the JIT is made instead of 32.
- **A refused piece waits for memory** as a table's commit does (`CommitWithRecovery`):
  `eden_memory_recover_try` and the piece again, up to 100 times, 20 ms apart when nothing came
  back, with `code_mutex` let go meanwhile. Only then does the JIT give up.
- **The executable views lie side by side** in one span of address space of 2,032 MiB
  (`CodeSpan`, `TakeFromCodeSpan`): reserved with the first cache, a cache's view at the lowest
  free place in it, the place reserved again when the cache goes (`ReserveOver`, piece by
  piece) and the span given back with the last cache. Six caches of 320 MiB are 1,920 MiB, all
  within a 32-bit offset of each other. A cache that finds no room there, or no span, gets its
  executable view where the system has room, as before.
- 32 caches can grow this way at once instead of 16 (a cache with no entry is taken whole).
- A cache below 32 MiB is still taken whole. A cleared cache keeps its memory, as before and
  as in PS5_Lichii: every core recompiles the game's current code into it at once, and memory
  given back at that moment could be gone when the JIT asks for it again.

`Eglantine/src/core/arm/dynarmic/arm_dynarmic_64.cpp`: a Switch 2 title's six cores get
320 MiB each (1,920 MiB of address space instead of 688). A Switch 1 64-bit title keeps
256/192/192/128/128/128, and a 32-bit title 512/64/64/16/16/16 (`arm_dynarmic_32.cpp`,
untouched); both get the 2 MiB pieces and the span.

`headless/memory_check.cpp`: `CheckCodeRanges`.

## Switches (read once)

| `env.txt` | |
|---|---|
| `EDEN_JIT_LAZY_CACHE=0` | The caches as before this change: 160/144/96/96/96/96 MiB, 16 MiB steps, each view where the system has room, three requests for memory back and no wait. This is not "everything committed at once": that never was the default. |
| `EDEN_PS5_CODE_CACHE_MIB=<n>` | Every core's cache is `n` MiB, for any 64-bit title: 64 to 336, an even number (six of 336 MiB are the most the span holds). Ignored with `EDEN_JIT_LAZY_CACHE=0`. |
| `jit_memory=whole` in `dev-settings.txt` (was there) | Every cache whole, at once. With 320 MiB a core that is 1.9 GB: only with `EDEN_JIT_LAZY_CACHE=0`. |

## Memory, expected

For a Switch 2 title, against the caches as they were:

- **When the JITs are made:** 120 MiB instead of 192 MiB (72 MiB less).
- **While it runs:** each cache holds up to 14 MiB less for the same code (7 MiB on average, so
  about 40 MiB over six caches).
- **What the larger caches can cost:** nothing while no cache would have filled before. Where
  one would have (160 MiB of blocks compiled by core 0, 144 by core 1, 96 by another), all six
  were cleared and the game's current code compiled again; now that code stays, up to 320 MiB
  a core. The most is 1,920 MiB instead of 688 MiB, but the cores share blocks, so what is
  held is the game's code once, not once per core as in PS5_Lichii.

Not done, and worth measuring first: the 16 MiB commit when a JIT is made is far more than its
prelude takes (the constant pool is 2 MiB, the fallbacks an estimated 2 to 3 MiB). Trimmed, a
core that compiles little would hold 6 to 8 MiB instead of 20.

## Checks

- `tools/check-jit-allocator.py` passes unchanged (host backend and mocked native calls): its
  64 MiB cache now runs from the span, in 2 MiB pieces.
- `eden-memory-check`, `CheckCodeRanges`: two 320 MiB caches, no memory when made, memory in
  steps as the JIT asks, one memory behind both views, a fault past the memory and for a write
  to the view that runs, everything given back. The function was compiled alone against
  `src/memory_pages.cpp` on the host and passed with and without `EDEN_JIT_LAZY_CACHE=0`;
  `eden-memory-check` was built with it afterwards and passes.
- A scratch check with native calls mocked to honour `MAP_FIXED` and to refuse memory (not in
  the repository): six caches of 320 MiB side by side, a seventh outside the span, a piece
  that comes after three refusals, a cache leaving the span while the others run and the next
  taking its place, the span given back with the last, no direct memory left; and the old
  behaviour with `EDEN_JIT_LAZY_CACHE=0`.

## To check on the console

1. The log at a Switch 2 title's boot: six `EDEN_JIT_ALIAS bytes=335544320 active=1 code=...
   grows=1` lines whose `code=` addresses are 320 MiB apart, six `EDEN_JIT_FEATURES ...
   cache_bytes=335544320`, and `EDEN_JIT_MEMORY code=... held=20971520` for each.
2. A game's end and a second game in the same run of the app: the span is given back and made
   again (`ReserveOver` over pieces inside a larger reservation, and `munmap` of the span, are
   new here).
3. `EDEN_JIT_PRESSURE` lines (a full cache) with `EDEN_PS5_CODE_CACHE_MIB=64`, which fills
   early: the clear and the caches' reuse are as before, but had no run with these sizes.
4. Mario Kart World without Pro memory, into a race: `EDEN_JIT_MEMORY a piece found no memory
   and got it after asking` instead of "the console has no memory for its next part".
5. The same session with `EDEN_JIT_LAZY_CACHE=0`, for the frame rate and the pool's free memory.
