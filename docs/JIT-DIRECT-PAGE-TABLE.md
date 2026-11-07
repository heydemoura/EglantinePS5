# JIT direct page table (Phase 3a)

Backported from ProsperoEden (GPL-3.0-or-later). The code is `headless/jit-page-table.cmake`,
plus the sparse-vector mirror, JIT configuration and unmap hooks in `headless/CMakeLists.txt`.

On the console, fastmem is off for 64-bit games, so every JIT load and store goes through Eden's
packed page table. Each access decodes the entry (shl, sar, and) and tests the marked bit. Each
process page table now keeps a second array 1 GiB after its entries: per page, the host pointer
when the page is directly accessible, or 0 when the access must take the callback path (marked,
debug or unmapped). The JIT reads that value with no mask, no marked bit and no sign extension.

All entry writes (`Store`, `MarkRasterizerCached`, `MarkDebug`, the unmap's `ZeroRegion`) update
the mirror. A page that becomes slow is cleared before its entry changes, and a page that
becomes fast is set after, so the JIT is never more permissive than the packed entry. Committing
a table page also commits its mirror.

**On by default** (since Phase 3d). `EDEN_JIT_TABLE=0` in `env.txt` turns it off. The log prints `EDEN_JIT_TABLE direct=0|1` once.
The direct table needs Eden's sparse tables.

**Cost:** about as much table memory again as the entries in use (~0.24 GB on Mario Kart World,
where the console's pool is already short: docs/MKW-MEMORY-BUDGET.md).

## Validation

- `eden-memory-check` runs the whole JIT suite with the direct table on, plus a new
  `CheckDirectPageTable`: 23 cases of entry and mirror invariants, the first write committing the
  mirror, unmap order, and A64 loads and stores through the direct table, including callbacks on
  marked, cleared, out-of-range and never-mapped pages. All checks pass.
- Console, Mario Kart World 1.8.0, 0.5x, warm caches, interleaved off/on, two sessions
  (`tools/mkw-bench-console.py`):

| Session | Off (FPS) | Direct (FPS) |
|---|---|---|
| A (200 s) | 25.98, — (log lost) | 20.80, 26.06 |
| B (180 s) | 20.58, 21.17, 16.19 | 18.62, 23.62, 30.30 |
| Median | **20.9** (4 runs) | **23.6** (5 runs) |

The title's frame rate varies with the attract scene and with how soon the memory crash comes,
so the spread is wide. Direct won two of the three interleaved pairs in session B. The result is
positive but not yet conclusive, so the table stays opt-in until a longer qualification. No new
crash type or rendering fault appeared (screenshots checked). All runs but one still end in the
known pool exhaustion.
