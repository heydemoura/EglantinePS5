# 64-bit fastmem window (Phase 3e, opt-in)

Backported from ProsperoEden (GPL-3.0-or-later) and adapted to Eglantine's guest memory service and
code-cache protection. **Off by default; `EDEN_FASTMEM64=1` (`env.txt`) turns it on.**

## What it does

64-bit games get a fastmem arena: a 2^38-byte reserved window
(`src/host_memory.cpp`, `Window64`). The window shows nothing until a JIT access faults there.
The fault handler then shows the guest's memory and the access repeats:

- **Whole region:** a 2 MiB region whose 512 pages map one 2 MiB-aligned slot with the same
  access shows the slot as one large page (`AliasGuestSlot`).
  - Own slot: read-write.
  - Zero or Shared slot, for a read: its block, read-only.
- **Pieces:** otherwise the 16 KiB piece around the access is shown (`AliasGuestPage`), when its
  four pages are mapped, in line in the backing and 16 KiB-aligned there. Each region shown this
  way uses one CPU page table; new ones stop while fewer than 1,024 page tables are free.
- **Writes to Zero, Shared or Fine slots:** the slot is promoted first, as a write to the backing
  would be, and then shown read-write.
- **Anything else** falls back. dynarmic moves the access site onto the page-table path (a
  "demotion").

## Safety

- **Alias drops:** the guest memory service drops a slot's window aliases, under the slot's lock,
  before any change of its block or protection: promotions, returns, the scanner's fence, and
  release. `HostMemory` map, unmap and protect calls update the window's per-page record and drop
  regions that are no longer consistent.
- **Range check:** A64 JITs using the window range-check to 2^38, even in Auto accuracy (which
  otherwise disables the fastmem range check), so no access can reach past the window.
- **Fault callback** (`headless/dynarmic/fastmem-callback.cmake`, A64 unit only):
  - It no longer writes code. Upstream's `InvalidateBasicBlocks` from the fault handler tripped the
    code buffer's write-protection assert, and the game never rendered.
  - It records the site in a process-wide demotion set, so every core compiles it off the window.
  - It requests a deferred invalidation of the block on the faulting core. The other cores are not
    halted for it; a core still in the old block faults again and takes the fallback.
  - A32's checked fastmem keeps upstream's handling.
- **Saved list:** demoted sites are saved per title
  (`user/cache/jit/<title>.fastmem64-sites`, relative to the game's code, tied to its build ID).
  A later session starts with them instead of learning them through faults again.
- **Report:** every 5 s an `EDEN_FASTMEM64` line in `heap.log` counts served and refused faults by
  reason.

## Validation

- `eden-memory-check` adds `CheckWindow64`. On the host the window can never show guest memory,
  so this covers the fallback path, not the aliasing itself: 20 load/store cases in mapped,
  read-only, unmapped and past-2^38 regions, each through the fault and fallback and again after
  demotion, plus the window's map/protect/unmap tracking. All existing checks still pass with the
  A64 callback change.
- Console, Mario Kart World 1.8.0 at **1x**, 110 s runs (they end before the known memory crash so
  the list is saved), interleaved:

| Pair | Window (FPS) | Defaults (FPS) |
|---|---|---|
| 1 | 13.22 | 22.98 |
| 2 | 16.94 | 21.64 |
| 3 | 20.93 | 28.27 |
| Median | **16.94** | **22.98** |

The window renders correctly, and none of these runs crashed. The saved list works: 556k–570k
sites loaded, and refused faults fell from 562k in the learning session to 11–25k. **It is still
slower than the defaults on this title:**

- In the learning session, ~361k refusals were pieces whose backing is not 16 KiB-aligned with the
  guest address. The console maps 16 KiB pages, so they can never be shown, and ~162k more were
  pieces with an unmapped page. Most of MKW's memory accesses therefore stay on the page-table
  path.
- With a fastmem arena, the page-reuse pass (Phase 3b) is off, so those accesses lose its grouping.

## Follow-ups (stacked PRs)

| Step | PR | Change | Effect on MKW (1x) |
|---|---|---|---|
| 1 | #61 | Page reuse for demoted sites; no-arena "unwritten page" read rule; diagnostics | ~95k reuse groups; window 18.8 vs 24.8 FPS |
| 2 | #62 | Read-trap disarm without the global lock where none is armed (core patch 0020) | ~12% guest-core lock wait gone; 21.6 vs 26.6 FPS |
| 3 | #63 | Stacks get pages that agree with the window (core patch 0021, ProsperoEden 0060); list format EF65 | misaligned refusals 119k–361k → 1–2k |
| 4 | this | Pieces with an unmapped page are shown (`EDEN_FASTMEM64_STRICT_PIECES=1` restores the strict rule); list format EF66 | unmapped refusals ~450k → 0; saved list 500k → 40k sites |

After step 4, with a list learned under the current rules, the window **matches the defaults**: 22.5 / 23.6 /
23.9 FPS against 22.4 / 22.9 FPS (the third defaults run crashed before its measurement). The warm-up
session reached 26.2 FPS. Remaining refusals per session are small: about 100–1,100 partial, ~2,000 slot
(Fine slots) and a few hundred blocked by GPU write tracking.

**Bump the saved list's format (`EF66` in `fastmem-callback.cmake`) whenever the window's rules change:**
a list learned under older rules keeps sites off the window that would now work.

## Still open

- **Fine slots:** reads of Fine (partly zero) slots are refused, so their sites are demoted.
- **More titles:** qualify the window on another 64-bit title.
- **Default:** the window stays opt-in until it beats the defaults by more than run-to-run noise.
