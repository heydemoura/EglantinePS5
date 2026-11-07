# Stability and performance plan

Next steps for EglantinePS5 after the Ampere shader work (PRs #26-#28), written 2026-11-05 so a
later session can pick any item up without the history. Each item has a goal, what is already
known, a spec, acceptance criteria and how to measure it. Items are in priority order; take them
one at a time, one branch and PR per item, and update the **Status** line when one moves.

## Ground rules (all items)

- **Evidence**: judge a console run with the console's own logs (klog, port 3232), the emulator log
  (`/data/eglantineps5/user/log/eden_log.txt`), crash reports (`/data/eglantineps5/logs/`) and the
  console's own screenshots (system screenshot, or a frame of ps5-web-stream
  `http://192.168.0.190:8080/live.flv`). Never conclude from one source.
- **Console access** goes through `tools/console-lease.py --host 192.168.0.190 run --wait -- CMD`;
  the console is shared with other sessions. Restore the installed eboot, `env.txt`,
  `compat-input.txt` and `dev-settings.txt` after every experiment.
- **Builds**: always `tools/prepare-build.sh` then `tools/build-package.sh release` inside the
  `prospero-win` container (see `BUILDING.md`). Driving a release build needs the input-enabled twin
  (temporarily prepend `#define MKW_DIAGNOSTIC_INPUT 1` to `headless/main.cpp`; never commit it).
- **Defaults**: new behaviour lands behind an `EDEN_*` switch, is measured, then becomes the default
  only if the measurement says so. A switch that can take the console down stays off.
- **Hard constraints**: no paging guest memory to the SSD (stutter, SSD wear); no shared RADV/Mesa
  checkout edits for experiments (use a private copy under `.local/`); keep fallbacks to the
  existing path for anything new in the shader pipeline.

## 0. Housekeeping (do first)

**Status**: open.

- PR #27 (phase 2 + the merged phase 3) still targets `perf/ampere-native-lowering`, already merged
  into `main`. Retarget it to `main`, check the patch series applies after 0013, merge.
- Smoke-test current `main` on the console: MKW reaches the title, Bananza reaches the cave and
  renders (fresh shader cache: about 10 minutes). Record build hash and results here.
- PR #20 (MKW memory fit, draft) targets `perf/mkw-core-draw-path`; rebase it on `main` before
  item S1 continues from it.

## S0. A repeatable measurement harness

**Status**: open. **Why first**: every later item needs an A/B that a single run cannot give.
Bananza FPS drifts with the scene; MKW's title FPS changes when anyone touches the menus (menus run
at 60 FPS); PGO's A/B came out inconclusive for that reason.

**Spec**
- One driver, `tools/bench/run.py TITLE CANDIDATE --runs N --env ...`, built from
  `.local/mb/run-bananza.py` and `tools/mkw-title/run.py`: installs the candidate, writes `env.txt`,
  launches, waits for a fixed scene, measures, collects evidence, restores the console.
- Fixed scenes: MKW title hands-off for 20 s after the title appears; Bananza "Meet up with
  others" save, cave, using the recorded-input replay (`gameplay-replay.txt` sidecar) instead of
  live steering.
- Output per run: frames, seconds, mean FPS, worst frame ms, 1% low, `NX2_COMPILE_TIME` totals,
  crash or not, survival seconds, screenshots at fixed times. Output per candidate: mean and
  standard deviation over runs, and a verdict only when the difference exceeds 2 standard deviations.
- Shader-compile scenario: fresh shader cache, total driver and emitter time from
  `EDEN_NX2_COMPILE_TIMES=1`, plus the count of frames over 50 ms during the first 3 minutes
  (stutter).

**Acceptance**: three runs of the same build give FPS within ±3% of each other in both titles.

## S1. Mario Kart World memory fit (OOM after 2-5 minutes)

**Status**: open; experiments and draft PR #20 exist. **The largest user-visible problem.**

**Known** (console measurements, 2026-11-04)
- The PS5 direct pool is 12,224 MiB (13,440 with the PS5 Pro memory bit 22 in `param.json`
  attribute3). At the MKW crash it is full: guest memory the game wrote about 4.5 GB, host heap
  about 2.1 GB, GPU driver about 3.7-3.9 GB, JIT code about 0.3 GB, sparse tables 0.2 GB.
- The guest's growth is real: the game's JIT keeps allocating and writing (about 440 MB per 30 s at
  the title) and never frees. No single leak; the fix is on the host side.
- Before the crash, image allocations fail and fall back to unbacked sparse images
  (`vulkan_memory_allocator.cpp:344`): that is the texture corruption seen just before the crash.
- GPU memory at 80 s: render targets 940 MB in 12 dedicated allocations (largest 341 MiB, an
  8192² RGBA8 image with mips), sampled textures about 0.62 GB, staging 2×95 MB plus 128 MB, buffer
  cache about 213 MB, about 510 MB unused inside 64 MiB VMA blocks.
- Already tried and rejected: SSD pager (stutter), `EDEN_GUEST_HIGH` (device lost, console down),
  dedicated allocation per image (froze), dropping guest copies of textures (guest read zeroes and
  crashed), age-based freeing of GPU blocks (every block has an image in use each frame), smaller
  or larger VMA blocks (worse). LZ4 RAM pager: smooth, survives longest (about 270-310 s), then dies
  of real exhaustion.

**Spec** (in this order; each a separate measured step)
1. *GPU reads guest memory directly* (the option chosen on 2026-11-04): map guest memory into
   the GPU address space instead of copying it into Vulkan buffers. Import the guest backing pages
   into RADV (`VK_EXT_external_memory_host` or a PS5 winsys import of the direct-memory ranges) and
   point the buffer cache and staging at them. Target: buffer cache and upload staging (about
   0.5 GB) and the duplicated guest-side copies.
2. *Render-target diet*: find which guest surfaces become the 8192² RGBA8 targets and whether
   their mip chains are ever sampled; allocate mips lazily, and alias transient targets whose
   lifetimes do not overlap.
3. *VMA block reuse*: compact the 510 MB of nearly empty 64 MiB blocks by moving their few images
   when the texture cache is idle (VMA defragmentation API), instead of changing block size.
4. *Host heap audit*: attribute the 2.1 GB host heap per subsystem (the heap guard / arena
   statistics) and cut the largest consumer.
5. *Graceful degradation*: when an image allocation fails, run the texture cache's eviction and
   retry before falling back to an unbacked sparse image, and log it, so a near-full pool costs
   speed instead of corrupted textures.
6. Keep the LZ4 RAM pager (PR #20) as the last line, default off until steps 1-5 are measured.

**Acceptance**: MKW runs 30 minutes (title, menus and a race) on the base pool with no crash and no
texture corruption, FPS not lower than `main` by more than 5% in S0's harness.

## S2. Known races and freezes

**Status**: open.

- The deadlock found with PR #19: `nx2:gpu SubRegisterMemory` holds the `Nx2GpuMemoryRegistry`
  mutex while the GPU `ThreadManager::Run` blocks on its full 8-entry queue, and the GPU thread
  (Fermi2D software blit, `BackSparseWrite`) waits for the same mutex. MKW's patch 0004
  (`Nx2GpuCommandScope`, deferred `RunInOrder`) avoids it on the MKW branch. Spec: confirm
  that `main` has it for every title, then add a lock-order assertion (development builds) that
  fails when a thread waits on the GPU queue while holding the registry mutex.
- After S1 step 5, an allocation failure must never become wrong memory: audit every
  `vkAllocateMemory`/VMA failure path for fallbacks that hand out unbacked or zero memory.

**Acceptance**: 10 consecutive MKW and Bananza boots without a freeze; the lock-order check is
silent in a 30-minute run.

## P1. NIR backend coverage

**Status**: open; needs PR #27 merged (it carries phase 3). Phase 3 (`headless/nir_backend.cpp`,
`docs/ampere-nir-backend.md`) translates 2
of Bananza's 21 compute programs; the others fall back to SPIR-V.

**Spec**
1. The three missing operations, which cover every compute fallback seen in Bananza:
   `GetCbufU32x2` (14 programs), `FPRecip32` (4), `LoadStorage64` (1). Copy their semantics
   from the SPIR-V emitters (`backend/spirv/emit_spirv_*.cpp`) and from what `spirv_to_nir` makes
   of them, including float-control flags.
2. Log MKW's compute fallbacks the same way and translate what they need.
3. Graphics stages (vertex, fragment): inputs and outputs as variables with the SPIR-V backend's
   locations, interpolation modes, discard/demote, render-target outputs, the vertex shader's
   depth conversion. Larger: one stage per step, fragment first (most numerous).

**Acceptance**, per translated program: driver statistics within 0.5% instructions and equal
VGPRs/LDS against the SPIR-V build (`EDEN_NX2_PIPELINE_STATS=1`); screenshots match; no new crash.
Report the share of programs on NIR and the total compile-time change in S0's shader-compile
scenario.

## P2. Shader compile stutter

**Status**: open.

**Spec**
- Measure first (S0): how many frames over 50 ms during the first minutes on a fresh cache, and
  which pipelines were compiling then.
- Make sure RADV's own disk cache persists on the console between launches (location and size),
  so a second launch compiles nothing.
- Compile pipelines asynchronously where the emulator allows it (draw with a pending pipeline
  skipped or deferred), behind a switch, for graphics pipelines.
- Pre-warm: compile the emulator's stored pipelines in the background at boot, in priority order
  (most recently used first).

**Acceptance**: the second launch of a scene has no compile hitches; the first launch has fewer
frames over 50 ms than `main` in S0.

## P3. HMMA B fragments without LDS

**Status**: investigate. The Bananza HMMA shaders' B-fragment shuffles (lane `8(t%4) + 4c + k`)
cross quads and compile to `ds_bpermute` (LDS round trips). RDNA has `v_permlane16_b32` /
`v_permlanex16_b32`. SPIR-V could not express them; the NIR backend can emit a target intrinsic.

**Spec**: check which permute intrinsics Mesa's NIR and ACO expose for GFX10 (GFX1013 is the PS5
GPU); if one fits, emit it in the NIR backend for the HMMA B shuffle pattern only.

**Acceptance**: fewer LDS instructions and lower latency in the two HMMA shaders' statistics,
identical rendering.

## P4. CPU side

**Status**: later.

- Retry PGO (the toolchain work is done: profile runtime, PROSPERO guards) once S0 gives a
  trustworthy A/B; profile MKW's title and a race, Bananza's cave.
- Sample the CPU cores (ps5-perf payload) in MKW to see whether the JIT, the GPU thread or
  synchronization is the limit before changing anything.

**Acceptance**: a gain beyond S0's noise in at least one title, no loss in the other.

## Reference

- Console: 192.168.0.190 (elfldr 9021, app FTP 2121, ps5-web-stream 8080, klog 3232).
- Switches already in the tree: `EDEN_NX2_HMMA_NATIVE`, `EDEN_NX2_NIR`, `EDEN_NX2_COMPILE_TIMES`,
  `EDEN_NX2_PIPELINE_STATS`, `EDEN_NX2_IR_STATS`, `EDEN_NX2_IR_OPCODES`,
  `EDEN_NX2_SHADER_DUMP_ALL`.
- Related documents: `docs/MKW-TITLE-FPS.md`, `docs/MKW-PERFORMANCE-REVIEW.txt`; on the Ampere
  branches `docs/ampere-rdna2-mapping.md` and `docs/ampere-nir-backend.md`.
