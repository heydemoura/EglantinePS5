# Eden heap: what it holds and how to make it smaller (2026-11-13)

Worktree `~/Code/EglantinePS5-wt-zmem`, branch `perf/heap-memory` (measured on `research/heap-memory`, which sits on the Zelda research branch: its baseline runs include those research switches, all off by default).
Zelda: Ocarina of Time .nup, PS5 Pro, 1x, new save into gameplay on Link's porch, 240 s runs, warm and cold
pipeline cache. Heap figures are the app's own report (`heap: holds … MiB, … of it in use`); places come from
`EDEN_HEAP_SITE` (sampled 1/128 + every block of 1 MiB or more), named with `.local/heap/sites.py`.

## Summary

- **Steady play: −245 to −270 MiB in use, −190 to −290 MiB held, with the same picture and frame rate.** Three
  fixed tables were far larger than anything uses (descriptor staging, the CPU JIT's dispatch table, the GPU
  page-state table). Prototype `candidate-zm9`: heap 1,035 / 1,037 MiB in use (baseline 1,279–1,321), held
  1,352–1,384 (baseline 1,576–1,640), steady free pool 728–872 MiB (baseline 607–792), 29.5–30.1 FPS (baseline
  29.0–30.1). Screenshots of the porch are the same.
- **The big remaining cost is slack left by shader compilation.** On a cold pipeline cache the heap holds
  2.3 GB with under 1 GB in use at the peak, and still ~1 GB of slack after the trim. The 8 `VkShaderDriver`
  threads churn tens of GB of RADV compiler memory (`ralloc`) and grow to 231 arenas (462 MiB) holding 83 MiB.
  What keeps those arenas from going back is RADV's long-lived per-pipeline objects, allocated by the same
  threads between the compiler's short-lived blocks.
- **Smaller arenas do not help** (512 KiB instead of 2 MiB): the arenas held the same, the heap's own spaces
  fragmented more (held 2.63 GB against 2.02), and the cold run ran the pool out (crash at 104 s). Reverted.

## Where the heap goes (warm, steady, before the changes)

1.3 GB in use. The largest places:

| MiB | What | Note |
|---:|---|---|
| 96 + 2×24 | `UpdateDescriptorQueue` host staging (guest + 2 compute), 8 frames each | peak use is 9–14% |
| 96 | Dynarmic A64 fast dispatch table, 16 MiB × 6 JITs | A32 uses 1 MiB |
| 64 | `Tegra::MemoryManager::entries`: 2 bits per 4 KiB page of the 40-bit GPU space, dense | the game maps a few GB |
| 47 + 24 | `KDynamicPageManager` (emulated kernel) | Switch kernel resource sizes |
| 35 | `KMemoryManager` | |
| 34 | NS shared fonts | needed |
| 33 + 16 + 16 | Dynarmic block registry (`JitGroup::Register`, 744k blocks), `BlockRangeInformation`, per-JIT state | grows with code run |
| 31 + 13 + 14 + 8 | NUP container index, file tree, RomFS concat | per file entry |
| 19 | `Core::System` | |
| 16 + 16 | `DeviceMemoryManager`, `TextureCache` | |

## Changes measured (prototype, behind `#if defined(__PROSPERO__) // PS5`)

1. **Descriptor queues start smaller** (`EDEN_DESCRIPTOR_START_DIV=8`, `vk_update_descriptor.cpp`): host storage
   starts at 1/8 and grows on demand to the same ceiling as before (4× the original size). On the porch the
   guest queue peaks at 76–80k entries a frame, so it grows once, to 131,072 (24 MiB instead of 96); the compute
   queues peak at 12–14k of 16,384 (3 MiB each instead of 24). ~114 MiB less. The one growth waits for the GPU
   worker once (at load). Shipping version: start the guest queue at 0x20000 and the compute queues at 0x8000
   (no growth on this game, 30 MiB in all), and keep the growth path.
2. **A64 fast dispatch table 1 Mi → 64 Ki entries** (`a64_emit_x64.h`, the size A32 already uses): 90 MiB less.
   A miss costs one block lookup; no FPS change measured (29.3–30.1).
3. **GPU page-state table sparse** (`memory_manager.h/.cpp`: `Common::SparseLargeVector<u64>`, the PS5 sparse
   ranges back only the 2 MiB slots that are written): ~60 MiB less. Reads of untouched pages see zero (Free),
   as before.

| Run | Build | Heap held | In use | Steady free | FPS |
|---|---|---:|---:|---:|---:|
| baseline ×7 (base1, m2base1–3, zm4base, drv1–2) | main + research switches | 1,576–1,640 | 1,279–1,321 | 607–792 | 29.0–30.1 |
| hp1 | 2 + 3 | 1,416 | 1,124 | 773 | 29.3 |
| hp2div8 | 1 + 2 + 3 | 1,384 | 1,033 | 872 | 29.5 |
| hpfin1 / hpfin2 | 1 + 2 + 3 (zm9) | 1,384 / 1,352 | 1,035 / 1,037 | 822 / 728 | 30.1 / 30.1 |

## Cold pipeline cache: the compile slack

Cold run with the changes (hpcold1, zm6): heap 431 → 2,312 MiB held at the load peak with 956 in use; after
the trim (864 MiB given back) 2,024 held, ~1,045 in use. Load-window minimum free pool: 64 MiB (the earlier
cold runs: 12–114).

New diagnostics (`heap_arenas.inc`, printed with the heap report): `EDEN_HEAP_THREADARENAS` (threads with more
than one arena), `EDEN_HEAP_FLOW` (bytes ever allocated by place) and `EDEN_HEAP_PIN` (live blocks inside arenas
under a quarter full).

- At the peak, 8 `VkShaderDriver` threads hold 5–66 arenas each (231 arenas, 462 MiB) with 0–36 MiB live each.
- `EDEN_HEAP_FLOW`: `ralloc_size` (NIR/ACO compile memory) is 23 GB allocated by the peak and 37 GB by the end
  (all `ralloc` callers fall into one place: the site key is the allocator frames).
- `EDEN_HEAP_PIN`: what stays in the near-empty arenas is RADV's long-lived objects: `radv_shader_create_uncached`
  (via `radv_shader_deserialize`/pipeline cache lookup), `radv_parse_binary_debug_info`, descriptor set
  layouts, VS prologs. They are allocated with plain `calloc`/`malloc` (not `VkAllocationCallbacks`), between
  compiler blocks that are freed at the end of each build.

### What would reduce it (next steps, by expected gain)

1. **Keep RADV's long-lived objects out of the compile threads' arenas** (estimate: ~0.4–0.9 GB after a cold load, and
   most of the peak slack). In the private RADV build, allocate `radv_shader` and the shader's debug info,
   descriptor set layouts and shader parts with a heap entry point that uses one shared "long-lived" arena set
   (`eden_heap_long_lived_calloc`, new in `heap_arenas.inc`); the compiler's blocks keep the thread arenas,
   which then empty after each build and go back through `eden_heap_release_arenas`. Needs a RADV rebuild
   (not done here: the shared RADV tree must not be rebuilt from this worktree).
2. **Drop RADV's per-shader debug info** when no debug option asks for it (`radv_parse_binary_debug_info`).
3. **Cap concurrent driver builds while the pool is short** (earlier: 2 workers instead of 12 lowered the peak
   ~240 MB).
4. Not worth it: smaller arenas (measured worse), `EDEN_HEAP_ARENA_KEEP=0` (no effect).

Smaller fixed items left (13–47 MiB each): emulated kernel resource managers, NUP index (could keep offsets
only), Dynarmic block registry.

## Kit

- Builds: `.local/zmem/build.sh NAME` → `.local/zmem/candidate-NAME` (zm5: changes 2–3 + switch 1; zm6: + heap
  diagnostics; zm8: 512 KiB arenas, rejected; zm9: final).
- Runs: `.local/zmem/run.sh CAND NAME 240 --env EDEN_NX2_TC_LOG=1 --env EDEN_DESCRIPTOR_START_DIV=8`;
  cold: `zcache.py stash` / `restore` around the run (hp3.sh, hp4.sh).
- Places: `.local/heap/sites.py STDERR ELF [--tag EDEN_HEAP_SITE|EDEN_HEAP_FLOW|EDEN_HEAP_PIN] [--report N]`.
- Gotcha: the console's `pthread_getname_np` writes the whole name (up to 32 bytes) whatever the size passed; a
  16-byte buffer corrupted the next thread record (zm7 crashed in the heap).
