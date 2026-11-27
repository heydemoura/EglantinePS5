# Ocarina of Time on a standard PS5: where the 12 GiB pool goes

Status 2026-11-15, branch `zelda-memory` on v1.000.002 (`64dbcde`). Console: standard PS5
(12,224 MiB pool, no Pro memory), firmware 12.00. Game: OoT (0400486024DEC000) v131072, main build
id E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348, docked, 1x, Max Resolution and Texture Pool x2
add-ons off, `EDEN_NX2_NO_MESH=1` (without it the first task/mesh draw after the title logo hung
the GPU every time).

## Symptoms

- v1.000.002 as released: the process is killed by the system (no crash report), or SIGSEGV at
  `eboot+0x21e1629` (RADV `begin_subpass`, null attachment), 2–3 minutes in, at the first area
  change after the title.
- With this branch: no SIGSEGV; gameplay is reached and the first outdoor area loads, then the
  pool runs dry and the process is killed (runs 10–12, 1–4 minutes of play).
- Slow motion in gameplay, from two separate causes (see "Frame time" below).

## Accounting near the end of a run (run 11, 2 s `EDEN_NX2_MEM_TRACE` + RADV report)

| Holder | MiB |
|---|---|
| Guest memory held (written 2 MiB slots) | 4,700–5,100 |
| VMA device-local blocks (type 0) | 2,785 blocks / 2,326 allocated |
| – texture cache images (786; 710 MiB GPU-written render targets) | 1,180 |
| – buffer cache | 510 |
| – other VMA allocations (not in either cache's count) | ~640 |
| – block slack (allocated blocks, unused) | ~460 |
| VMA host-visible types (2, 3, 5) | ~190 |
| RADV shaders (p31) | 112 |
| Eden heap | ~1,500 |
| JIT code caches (Switch 2 per-core sizes 160/144/96×4/16×2 MiB) | ~450 held |
| Free | 1–360 |

Run 12 in handheld mode: render targets 690 MiB against 710 docked. Console mode does not change
the game's render target memory.

## Findings

1. **Guest memory barely compresses**: `EDEN_CRAM_COLD` lz4 ratio 0.80–0.83 (MKW 0.27). Packing,
   the lever that let MKW reach races, saves ~17% here; recovery packs 20–80 MiB per call and keeps
   being asked.
2. **Resolution below 1x adds memory**: a rescaled image keeps `original_image` and `scaled_image`
   (`vk_texture_cache.cpp`, `Image::ScaleUp`), and copies between them. 0.5x/0.75x is a cost, not a
   saving.
3. **Low texture-cache limits thrash**: with `EDEN_NX2_TC_*` at 1–1.5 GiB the collector stayed
   aggressive: 2,924 images created, 2,035 deleted, 736 downloaded back to guest memory per 600
   frames. (The pool-based "device usage" reads 5.4–5.7 GB in gameplay against a 4,096 MiB critical
   mark, so the collector is aggressive anyway once the guest is past ~4.5 GB.)
4. **Lost in the move to the in-repo core** (old patch series → `Eglantine/`): 0023 Vulkan OOM
   recovery, 0056 index-buffer span cap, 0059 the GPU-reach hook (`eden_guest_gpu_cached` was
   defined in the service but never called).
5. **Staging**: idle buffers over 32 MiB stayed up to 64 frames (one level swept per frame). One-off
   staging requests of 64–214 MiB happen during loads (`MKW_STAGING_MISS asked=224792640`).
6. **Decode images**: up to 256 MiB of finished transcode images kept 60 frames, with no release
   when the pool is short.
7. **Recovery retried slots that do not pack** on every call (no back-off, unlike the scanner).
8. **The RADV memory report never ran**: it was tied to budget queries the pool-based collector no
   longer makes.
9. **The log lost its last lines**: the file flushed only on Error lines; an app killed by the
   system lost the seconds before.
10. **`EDEN_NX2_TC_LOG`'s 600-frame report did not print in runs 10–11** although the rasterizer
    ticked (the buffer cache's 60-tick line printed) and the switch was set; it did print in run 12.
    `EDEN_NX2_MEM_TRACE=1` (time-based) was reliable. Not chased further.
11. `EDEN_HEAP_SITES_LOST` is cumulative (frees are not subtracted), not live memory.
    `EDEN_HEAP_SITE` stacks do symbolize with `llvm-symbolizer` and the build's `llvm-pie.elf` once
    the stack-scan junk words (sizes such as `400000`, `c04000`) are dropped; no single heap site
    over 96 MiB.

## Frame time (why it plays in slow motion)

- **Memory stalls** (docked runs 10–11): once free memory is near zero, recovery runs on the GPU
  path; the emulator rendered only 3–23 of the game's 60 presents a second, one window of 60
  rasterizer ticks took 136 s, and the app froze before it was killed.
- **CPU-bound guest code** (run 12, ~800 MiB free, no recovery calls): presents average 37–50 ms
  (the game wants 33 ms), the present never waits on the GPU fence (2–66 µs), and render-thread
  pipeline builds are 4–170 ms on first visits. This matches `docs/MKW-TITLE-FPS.md`: the guest's
  code has to run faster; the renderer is not the limit. `EDEN_FASTMEM64=1` was not tried here
  (`docs/FASTMEM64-WINDOW.md` measures it at parity on MKW).

## Changes on the branch

| Commit | Change |
|---|---|
| `bd24db9` | Port of 0023 (VMA OOM recovery through `eden_memory_recover`, up to 3 retries), 0056 (64-bit index span, 128 MiB cap, oversized indexed draws dropped), 0059 (`eden_guest_gpu_cached` called from `DeviceMemoryManager` Map/Unmap and cached-count 0↔1) |
| `1bfa1ab` | `headless/performance.cpp`: RADV per-priority report with every `EDEN_MEMORY_PARTS` line |
| `47889ae` | `logging.cpp`: file backend flushes at least every 2 s |
| `ba0b4aa` | `Framebuffer::CreateFramebuffer` leaves out color targets with no image/view and a depth target with none (the RADV `begin_subpass` null deref at `eboot+0x21e1629` / `0x21e0f48`) |
| `98a4e1c` | Staging: idle buffers over 32 MiB released every frame while the pool is not short; VMA per-type and kept-staging lines at Info |
| `5282174` | Finished decode images dropped while the pool is short; recovery skips a slot that failed to pack for 10 s × 2^n (n ≤ 4) |

Each change was run on the console; together they removed the SIGSEGV and the immediate kills at the
first area change. None of them makes the gameplay fit in the pool.

## Rendering

- Most outdoor ground renders blue in gameplay (user report, runs 11–12). Not traced. Candidates: ASTC
  kept as BC3 (`astc_textures=full` is a development-build setting), or draws skipped by
  `EDEN_NX2_NO_MESH=1` (the logged skips were 16×16 and 128×128 targets).

## Open

- ~640 MiB of VMA allocations not counted by either cache, and ~460 MiB of block slack in 16 MiB
  blocks. Per-allocation tagging would say which (image alignment/padding, descriptor buffers,
  scratch and stream buffers are the candidates).
- JIT: per-core caches for Switch 2 hold ~450 MiB; capping them costs recompiles.
- Guest code speed for the 33 ms frame.
