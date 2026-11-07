# PS5 performance and stability plan: progress

Status as of 2026-11-06. The plan backports ProsperoEden's improvements to EglantinePS5 and
measures them on the console with Mario Kart World 1.8.0. Since Phase 3b, every run is at full
resolution (1x).

## Done

| Phase | What | PR | Default | Result |
|---|---|---|---|---|
| 0 | Repeatable console benchmark (`tools/mkw-bench-console.py`), memory-bounded build jobs | #41 (merged) | — | Title 23–26 FPS at 0.5x; no regression since 11-04 |
| 1a | Shared memory-pool accounting, RADV per-purpose memory report | #43 (merged) | report off | Every crash is the 12 GiB pool running out (docs/MKW-MEMORY-BUDGET.md) |
| 3a | JIT direct page table | #44 (merged) | on (3d) | 23.6 vs 20.9 FPS median at 0.5x |
| 3b | JIT page reuse | #46 | on (3d) | No measurable gain at 1x |
| 3c | JIT fast paths (FCMP, DMB, CheckBit, frame once) | #47 | on (3d) | **29.8 vs 22.6 FPS** median at 1x |
| 3d | 3a–3c on by default; `=0` switches turn each off | #48 | — | Defaults 24.6/24.5 vs all off 21.3/24.7 FPS at 1x |
| 3e | 64-bit fastmem window, saved per-title demotion list | #49 | **off** (`EDEN_FASTMEM64=1`) | Correct, but 16.9 vs 23.0 FPS at 1x |

This PR contains the whole stack, #46 → #47 → #48 → #49. Merging it supersedes those four PRs.
Each phase's details and measurements are in its doc: `docs/JIT-DIRECT-PAGE-TABLE.md`,
`docs/JIT-PAGE-REUSE.md`, `docs/JIT-FAST-PATHS.md` and `docs/FASTMEM64-WINDOW.md`.

## Findings that changed the plan

- **Memory crash.** Every run ends within about 2–3.5 minutes. The cause is one shared 12 GiB pool:
  guest RAM (~4.9 GB, still growing), the driver (~4.2 GB), Eden's heap (~1.9 GB) and about 1 GB
  more. The texture collector is already pool-aware, and lowering its limits does not help. BC3
  re-compression of ASTC was already in Eglantine. ProsperoEden's guest-memory states save page
  tables, not memory. The crash was set aside to work on Phase 3.
- **The limit is guest CPU.** The title's frame time is the guest's serial job chain, so JIT
  improvements (3a, 3c) moved FPS and renderer knobs did not.
- **Fastmem window.** Most of MKW's memory accesses cannot use it: their backing is not 16 KiB-aligned
  with the guest address, and page reuse is off while an arena exists.

## Open items

1. **Memory crash** (parked). Leads:
   - guest RAM growth through the title
   - descriptor/upload buffers that keep growing (+1.5 MiB/s)
   - VMA block fragmentation (~0.5 GB)
   - Eden heap growth
2. **Fastmem window follow-ups:** page reuse for demoted sites while the window is on, and 16 KiB
   placement of guest memory.
3. **Title capture:** with the fast paths on, the console's capture service often had no frame
   about 45 s after load, while the game kept rendering. The cause is unknown; check streaming
   before relying on it.
4. **Desktop build:** `eden-headless` does not link (`mkw_native_block` undefined; predates this
   work). The host checks need `libdrm` dev and `autoconf`.
5. **Phase 4 candidates:** skip texture-cache locks for pages that never held an image
   (`image_pages.h`), and a second title to qualify the JIT defaults.
