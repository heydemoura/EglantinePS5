# Mario Kart World: memory hotspots on the current core (2026-11-17)

Status: **research**. Branch `research/mkw-memory-hotspots`, from `main` at 3d5b3d7 (in-repo core,
upstream babdb6bb75 + PS5 edits). The code changes on the branch are diagnostics, plus two research
switches that are off by default. They are not meant to merge as they are.

What fills the console's 12 GiB direct-memory pool while MKW races, and what that costs the
emulator in time. It updates `MKW-MEMORY-BUDGET.md` and `MKW-MEMORY-LEAK.md`, which were
measured on the old core.

## How it was measured

- **Console:** PS5 Pro 192.168.0.190, MKW `.nup`. Each run boots, presses L+R and A through the
  menus into a Grand Prix, then holds A in the race for 420 s.
- **Resolution:** 1x.
- **Kit:** `.local/mm/` (git-ignored), from wt-core's `.local/mkw` kit:
  - `run.sh CAND NAME SECS [ENV…]`: one run, plus ps5debug map samples every 20 s;
  - `analyze.py`: pool, driver and guest timeline;
  - `fps.py`: presents per second, from the `nx2:vi2 present #N` lines.
- **Traces on in every run:**
  - `EDEN_NX2_MEM_TRACE=1`: texture cache, buffer cache and VMA every 2 s;
  - `EDEN_NX2_TC_LOG=1`: texture cache churn every 600 frames;
  - `EDEN_CRAM_COLD_REPORT=1`: cold guest memory;
  - the always-on `EDEN_MEMORY` and heap-site lines.
- **Runs:** six of 420 s, five to the race and one crash. In all five that got there, the race was
  rendering correctly at the end (screenshot at 390 s: Mario, 3/3 laps, HUD F33).

| Run | Candidate / switches | End | Presents/s (250–400 s) |
|---|---|---|---|
| base1 | main | ran 420 s | 31.1 |
| mirror1 | main + mirror hook | ran | 32.4 |
| diag1 | + image host-size trace | ran | 32.6 |
| pack1 | diag + `EDEN_CRAM_PACK=1` | ran | 32.0 |
| poolA | + pool-free walk (switch off) | **OOM crash at 58 s** (race load) | – |
| poolT1 | `EDEN_POOL_FREE_TOTAL=1` | ran | 33.3 |
| poolS1 | `EDEN_POOL_FREE_TOTAL=1 EDEN_POOL_SHORT_MIB=256` | ran | 33.0 |

The poolA crash is the known load-time pool exhaustion on the current core:
- `SIGSEGV 0x48` on VulkanWorker, in `transition_view_mask` (vk_render_pass.c:1706), under
  `begin_subpass`;
- an attachment whose image could not be allocated;
- largest free block 1 MiB.

The runs had main's defaults (that switch was off). So current main still dies at race load about
1 run in 6.

## Where the pool goes in a steady race (≈360 s)

Total direct memory: 13,440 MiB. Figures are from diag1/poolT1 at about 360 s and agree across
runs within ~5%.

| Holder | MiB | Notes |
|---|---|---|
| **Guest (game) memory held** | **4,500–4,800** | 2 MiB slots the game has written. 572 slots are kept at 4 KiB grain (`fine`, 355 MiB of 1.1 GB). 1.8–2.2 GB of the held memory was not written in the last 120 s. Its LZ4 ratio is 0.645 (sampled). |
| **Driver (RADV) total** | **4,450–4,600** | `EDEN_MEMORY_PARTS driver_mib` |
| ├ VMA device-local blocks | 4,100 | 3,600–3,800 in allocations, **300–450 empty inside blocks** |
| │ ├ texture images (real host bytes) | 2,600–2,800 | New trace, see below. The texture cache's own figure says 1,420, because it counts guest size. |
| │ ├ buffer cache | 490–510 | 1,133 buffers. The largest is 24 MiB. |
| │ └ other VMA allocations | ≈450 | staging/stream buffers, query and descriptor memory: not split yet |
| ├ VMA host-visible | 130 | |
| └ outside VMA | ≈300 | shaders, command and descriptor upload buffer objects |
| **Eden heap** | **1,810–1,930 held, 1,430–1,560 in use** | 0.4 GB slack. It grows from 416 MiB at boot. |
| JIT, stacks, tables, libraries | ≈1,000 | from the earlier budget doc, not re-measured |
| **Free** | **700–850 in 850–900 holes** | **The largest block is only 14–30 MiB.** 500–650 MiB of it is in whole 2 MiB pieces. |

## Hotspots, by size and by what they cost

### 1. The free pool is fragmented: 0.7–0.85 GB free, but the largest block reads 14–30 MiB

- **Evidence:** new `EDEN_MEMORY_FREE` line. It walks `sceKernelDirectMemoryQuery` once a second.
  - Free in steady race: 700–850 MiB in ~900 holes. The largest block is 14–30 MiB.
  - Already at 60 s (title → race load): largest 1,042 MiB of 2,980 MiB free.
- **Effect on the caches:** they read "memory free" as the *largest block* (`RefreshFreeMemory`,
  `ViewOfPool`).
  - So they see the pool as full all race long (`short=1`, device usage 6.1 GB against a 4.1 GB
    critical mark).
  - Meanwhile 0.5–0.65 GB is usable as 2 MiB pieces, which the driver can map since
    `tools/patch-radv-pieces.py`.
- **Effect on packing:** this is also why packing does not help.
  - pack1 packed 2,034 MiB of cold guest slots into 387 MiB (5.3:1).
  - Guest memory held fell ~0.5 GB, but the largest block and the cache churn did not change.
  - The slots it frees are scattered 2 MiB holes.
- **Where the holes come from:** the guest service takes and returns 2 MiB slots one at a time.
  The heap grows by 32 MiB pieces and trims. VMA frees and re-takes 16–64 MiB blocks. All of them
  share one pool with no zoning.
- **Ideas:**
  1. Zone the pool: guest slots from one end, driver and heap blocks from the other. Or have
     the guest service take its slots from a few large runs it owns.
  2. Make the caches' view count free 2 MiB pieces (`EDEN_POOL_FREE_TOTAL=1`, below), so their
     marks mean what is really left.

### 2. Texture cache churn in a steady race: ~175 images/s recreated (emulator time)

- **Evidence:** `EDEN_NX2_TC_LOG`, every run, 250–400 s, no scene change, holding A on one
  course.
  - Per 600 frames, the collector deletes 3,000–5,300 images, 60% of them "costly".
  - The draws re-create 3,000–5,600 images.
  - 560–800 MiB is re-uploaded from guest memory per 600 frames: ≈35 MiB/s of uploads, ASTC
    transcodes and image creates.
  - Almost nothing is downloaded (0–2), so this is pure re-creation.
- **Cause:** the collector is at its hardest the whole race.
  - Device usage on the console = budget − largest free block, scaled. That reads 5.5–6.1 GB
    against expected 3,584 and critical 4,096 MiB.
  - The game's image working set (~2.7 GB host) does not fit under those marks.
- **Tried:**
  - Feeding the collector the free 2 MiB pieces (`EDEN_POOL_FREE_TOTAL=1`): view free 500–650 MiB
    instead of 14–30.
  - Plus a lower "short" mark (`EDEN_POOL_SHORT_MIB=256`).
  - Neither changed the churn: 3,000–4,700 deleted per 600 frames. Usage still reads 5.5 GB,
    because the marks come from the title's budget. The pool view only adds the "short" override.
  - FPS moved within noise: 33.0–33.3 against 31.1–32.6 presents/s.
- **Ideas:**
  - Measure what the churn costs in GPU-thread time before changing policy (`EDEN_NX2_GPU_PROFILE`
    or a Tracy capture of `TickFrame`/`DeleteImage`/`UploadImageContents`).
  - Then scale the expected/critical marks to the image bytes the pool can actually hold. Or keep
    images used in the last N frames out of the collector's reach, with a hard stop only when an
    allocation fails.
  - On BC250 the same churn was 30% of GPUBack (`BC250-MKW-HOTSPOTS` notes). It is likely a real
    cost here too.

### 3. ASTC 8×8 re-compressed to BC1/BC3: 3.5× the guest size

New per-format trace: the real host bytes of every image (VMA allocation + bound sparse memory),
next to its guest size. diag1 at ~360 s:

| Guest format | Images | Guest MiB | Host MiB | Note |
|---|---|---|---|---|
| ASTC_2D_8X8_UNORM | 417 | 146 | **510** | 2 bpp ASTC → 4 bpp BC1 / 8 bpp BC3: **+364 MiB** |
| R16G16_UNORM | 4 | 1,364 | 410 | sparse, partly bound |
| ASTC_2D_8X8_SRGB | 304 | 3,001 | 406 | the 8192×8192×68 virtual-texture arrays, sparse/trimmed |
| BC6H_UFLOAT | 29 | 1,713 | 346 | sparse, partly bound |
| A8B8G8R8_SNORM | 5 | 286 | 286 | |
| D16 / RGBA8_SRGB / B10G11R11 / RGBA8 / D24S8 | | | 49–131 each | 1:1 |

- **Totals:** 2,580–2,800 MiB host for 1,322–2,263 images.
- **The texture cache's own figure is wrong:** its "1,420 MiB" in `Memory trace: texture cache`
  counts `GetScaledImageSizeBytes`, the guest size. That understates ASTC images 2–4×. It also
  counts sparse images at full size in the 600-frame line ("8,800 MiB").
- **Ideas:**
  - For ASTC alpha textures whose alpha is only 0/255 (cut-outs, foliage), use BC1 with 1-bit
    alpha: 4 bpp, half of BC3. The opacity scan already reads every new ASTC image; let it also
    tell binary alpha apart.
  - For ASTC ≥ 8×8, a lower-rate target (BC1 for all opaque, as now) is the floor without visual
    loss. Dropping the top mip of large ASTC 8×8 images would halve again, with visual loss.
  - Make the collector count host bytes. It then evicts by what it actually frees.

**Tried (branch `perf/astc-cutout-bc1`):**

- **Binary-alpha ASTC → BC1 punch-through** (`EDEN_NX2_ASTC_CUTOUT=1`, off by default). The scan
  decodes only blocks whose endpoint modes carry alpha, alpha channel only, and stops at the
  first in-between alpha; results are cached by content hash.
  - Cost: none worth counting. ≤23 ms of scans per 600 frames at race load, ~0 in the race
    (98% cache hits).
  - Gain: none. Only ~5 of ~520 alpha ASTC images per 600 frames are strictly 0/255. MKW's alpha
    textures have soft alpha. ASTC 8×8 UNORM host stayed 525 MiB (baseline 522–535).
  - A tolerance (`EDEN_NX2_ASTC_CUTOUT_TOL`) would catch more, but it hardens soft edges. It was
    not measured: the console's control port died before that run.
- **Trace lines count driver bytes** (PS5, always). The 600-frame line now reads ~2,650 MiB of
  images, where it used to read 8,750 MiB (guest size).
- **Collector counts driver bytes** (`EDEN_NX2_TC_HOST_BYTES=1`, off by default). One run:
  - churn 3,923 deletes per 600 frames, against 3,562–3,615 at baseline;
  - 32.0 presents/s, against 32.7–33.0.

  That is within noise, but there is only one run, so it stays opt-in.

### 4. Game memory: 4.5–4.8 GB held, ~2 GB of it cold

- The game writes fresh pages of its 8 GB heap as scenes change, and only returns ~0.6 GB a
  session to its kernel (`returns_free` 520–680).
- At ~360 s, 1.8–2.2 GB of it had not been written for 120 s.
- **Packing (`EDEN_CRAM_PACK=1`):**
  - Compresses the cold slots 5.3:1 in practice (2,034 → 387 MiB).
  - But 70% of packs are unpacked again (3,419 packs, 2,397 unpacks).
  - The freed slots land as scattered holes (hotspot 1).
  - Earlier sessions tied packing to garbage GPU draws (see `MKW-MEMORY-LEAK.md`). It stays off.
- **Running cost of the guest service:**
  - The scanner spends **22–27 s of CPU per 420 s session** (`scan_ms`, ~6% of one core).
  - Guest writes waited for a block **940–1,300 times** per session (`waits`). While one waits,
    the caches see no free memory at all (`GuestMemoryWanted`), which adds to hotspot 2.
- **Ideas:**
  - Pack in runs: pick cold slots that are physically adjacent, and give their blocks back as one
    run that the driver can use.
  - Scan less often while nothing is packed and the cold report is off. The pass costs ~0.4 s per
    pass at 65 passes.

### 5. Eden heap: grows 416 → 1,850 MiB; 0.4 GB slack; heavy allocation traffic

**Held** (`EDEN_HEAP_SITES`, followed blocks 1,154 MiB of 1,454 in use):

| Site | MiB | Blocks |
|---|---|---|
| FS IStorage read buffers (NUP members kept decoded, #118) | 108 | 5 |
| `DescriptorTable<TSCEntry>::Refresh` | 96 | 6 |
| `UpdateDescriptorQueue` | 96 + 72 | |
| `FileSys` / `ReadNativeDirectoryMetadata` (NUP index) | 94 + 32 | |
| `PipelineCache::CreateGraphicsPipeline` | 75 | 2,048 |
| `KDynamicPageManager` | 63 + 24 | |
| fiber stacks | 52 | 26 |
| `KMemoryManager` | 35 | |
| `ShaderCacheV2` program payloads | 32 | |
| Dynarmic `JitGroup::Register` | 30 | 669k |

The descriptor queues are the ones `perf/heap-memory` (#104) shrinks with
`EDEN_DESCRIPTOR_START_DIV=8`. That is still a switch on main, not the default.

**Allocation traffic** (`EDEN_HEAP_FLOW`, bytes allocated over the session, symbolized):

| Site | Allocated in 420 s |
|---|---|
| `ShaderPools` → `ObjectPool<IR::Inst>` chunks (`ShaderJobScheduler::RunCpu`) | **4,320 MiB** |
| `ObjectPool<IR::Block>` chunks (Maxwell translation) | **2,999 MiB** |
| `KServerSession::ReceiveRequest` (IPC buffers) | 1,376 MiB |
| FS `IStorage::Read` reply buffers | 1,337 MiB |
| shader optimization passes, `PrependNewInst` | 1,132 + 1,112 + 720 MiB |
| `nvhost_gpu::SubmitGPFIFO` | 1,082 MiB |
| `Nx2GpuMemoryRegistry::TrimMappings` / `MapAt` | 1,004 MiB |
| **`ZSTD_createDCtx` in `NupContainer::HasZstdMagic` ← `BuildDirTree`** | **831 MiB** |
| SPIR-V `Decorate` (`DefineTextures`) | 585 MiB |

Some shader job paths build new IR object pools: `ShaderPools pools;` locals at
`vk_pipeline_cache.cpp` 1268/1538/1769, rather than `TakeSparePools()`, each with an 8,192-entry
`Inst` chunk. The NUP directory build makes a fresh zstd context for every node it probes
(`HasZstdMagic`, nup_container.cpp:1047). That traffic is CPU time on the shader workers and the FS thread. It is also
what scatters the heap's arenas into the 0.4 GB slack the trim cannot give back.

**Ideas, cheap and safe:**
- Route the remaining `ShaderPools` locals through `TakeSparePools()`/`ReturnPools()`
  (`ReleaseContents()` keeps the chunks).
- Reuse one `ZSTD_DCtx` across `HasZstdMagic` probes (`ZSTD_DCtx_reset`). This is boot time and
  heap fragmentation, not the race.
- Reuse the IPC and FS reply buffers.
- Make the #104 descriptor start sizes the default.

## Not resolved

- **Guest pages mirrored by the GPU caches.**
  - The hook added to `DeviceMemoryManager::UpdatePagesCachedCountNoLock` (`EDEN_CRAM_MIRROR`)
    never fired: the service's table stayed empty and the debug print never appeared.
  - So the cache-count path is not reached for MKW's images on this core, or it reaches it with
    another address.
  - The old core's hook sat in `Map`. Next step: hook `Map` again, and count by image CPU range
    from the texture cache instead.
- **The ≈450 MiB of other VMA allocations, and ≈300 MiB of driver memory outside VMA:** not
  split. `RADV_PS5_MEMORY_REPORT` printed nothing on this build.

## Suggested order of work

1. **Heap traffic fixes** (hotspot 5: ShaderPools reuse, zstd context reuse, #104 defaults).
   Small, safe, and they help both CPU time and heap slack.
2. **ASTC binary-alpha → BC1** (hotspot 3). Up to a few hundred MiB of driver memory, no visual
   loss. Also make the collector count host bytes.
3. **Pool zoning / runs for guest slots** (hotspots 1 and 4). Turns the 0.5–0.8 GB of scattered
   free memory, and anything packing frees, into memory the driver can take. This is the
   structural fix for both the load-time OOM and the cache churn.
4. **Collector policy** (hotspot 2), once 3 is in and its GPU-thread cost has been measured.

## Changes on this branch

All of them are diagnostics or research switches. Nothing changes behaviour by default.

- **`headless/performance.cpp`:**
  - `EDEN_MEMORY_FREE` line: total free memory, free in 2 MiB pieces, hole count and the
    caches' view;
  - `EDEN_POOL_FREE_TOTAL=1`: the caches see the free 2 MiB pieces;
  - `EDEN_POOL_SHORT_MIB=<MiB>`: the "short" mark.
- **`texture_cache.h` (PS5):** with `EDEN_NX2_MEM_TRACE`, every 30 s, the real host bytes of the
  images by guest format (`Memory trace: images host …`). Uses `Image::ExtraMemoryBytes()`,
  added in `vk_texture_cache.h`.
- **`device_memory_manager.inc` + `src/guest_memory_service.inc`:** cache-count mirror hook and
  `EDEN_CRAM_MIRROR` line. It reads 0; see "Not resolved".
