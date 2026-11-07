# JIT page reuse (Phase 3b)

Backported from ProsperoEden (GPL-3.0-or-later). The code is `headless/dynarmic/page-reuse.cmake`
and `headless/dynarmic/page_reuse_pass.inc`.

With the direct page table (docs/JIT-DIRECT-PAGE-TABLE.md), each A64 load and store still checks
alignment, range-checks the address, loads the table entry and tests it: about ten x86
instructions per access. In hot blocks most accesses use one register plus small constants
(`[x12, #8]` … `[x12, #44]`). An optimizer pass gives each group within 256 bytes, with no
exclusive, ordered or exception-raising operation between, one `A64EdenPageLookup` of its lowest
address and span. The lookup returns the direct value when the whole span fits one page, and 0
otherwise. The group's accesses then emit `test`, `jz` and the access. On 0 they run the full
lookup out of line, and the callback only when that also fails.

The pass only runs with the direct table (no pointer mask, marked bit or sign extension, 8-byte
entries, a range-checked address space), without a fastmem window, and without halt-on-memory
checks.

**On by default** with the direct table (since Phase 3d). `EDEN_JIT_PAGE_REUSE=0` (`env.txt`) turns it off.

## Validation

- `eden-memory-check` adds `CheckPageReuse`: 914 cases, 1,324 groups of 4,409 accesses. Grouped
  and ungrouped results agree within a page, across a page boundary onto a scattered host page,
  on callback pages, and with post-indexed base registers. All other JIT checks still pass.
- Console, Mario Kart World 1.8.0, **1x**, warm caches, interleaved, 180 s each:

| Pair | Direct table only (FPS) | + page reuse (FPS) |
|---|---|---|
| 1 | 20.70 | 20.84 |
| 2 | 23.63 | 24.68 |
| 3 | 25.92 | 21.76 |
| Median | 23.63 | 21.76 |

**No measurable title-screen gain.** The run-to-run spread (±3 FPS from attract scenes and the
memory crash) is larger than a per-access saving could show here. The median worst-frame time was
lower with reuse in all three pairs (220/157/128 ms vs 283/213/205 ms), but that is not
significant on three runs.

Every crash happened with 1 MiB of the pool free: the known exhaustion (docs/MKW-MEMORY-BUDGET.md).
The SIGABRT was at the same address as the baseline abort, so these are not JIT faults. The pass
stays opt-in until a profile build can measure guest JIT time directly.
