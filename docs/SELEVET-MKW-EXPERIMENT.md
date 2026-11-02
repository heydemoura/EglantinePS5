# Selevet core experiment for Mario Kart World on PS5

Status: experimental findings. No gameplay speedup or sustained 60 FPS is established.

The experiment uses `sylvieistaken/selevet-eglantine` at
`e902aca643b4abfb8217c5df5bf5b7e822962ef9`, in the separate
`experiment/selevet-mkw` worktree. The starting PS5 checkout is
`ce763b18de2099fe9b9440815e4415523b6315ff`, with core
`3140648a668aaebf87bae2dea17512fdca125ba0` and the locally developed MKW
compatibility work captured before adaptation. This is an experimental port of
the fork, with the PS5 sparse-memory transaction and Ampere/task/mesh interfaces
retained. It is not an unmodified build of Selevet's Windows release.

PS5 has a [Zen 2 CPU](https://blog.playstation.com/2020/03/18/unveiling-new-details-of-playstation-5-hardware-technical-specs/).
The fork's [current build instructions](https://github.com/sylvieistaken/selevet-eglantine/blob/e902aca643b4abfb8217c5df5bf5b7e822962ef9/tuned/README.md)
use `x86-64-v3`, despite the repository description mentioning Zen 3. The port
exposes `EDEN_BUILD_PRESET=zen2` and `EDEN_NATIVE_LTO=ON` as separate experiment
controls. Their defaults preserve the existing `custom` preset and LTO setting.

## Results established so far

| Experiment | Observation | Scope |
| --- | --- | --- |
| GPU mapping correctness | 200,000 randomized operations plus zero/overflow cases matched the original updates and effective ranges | Host differential test; includes explicit mappings and native leases |
| GPU mapping benchmark | Original 111.667 µs/update; Selevet 3.09009 µs/update; 36.1× ratio | Synthetic host test with 1,024 mappings, 8,000 timed updates; not game FPS |
| Core regression suite | 171 cases, 2,239,770 assertions passed | Header/helper host checks; not complete rendering validation |
| Port reproduction | Clean checkout, root patch/gitlink, and all 131 modified/new core files reproduced | Optional snapshot; normal checkout keeps the existing default core |
| Native candidate | Build, all required source checks, package inventory/alignment/import checks passed | Clang 18; custom CPU preset; LTO off; temporary MKW timing/native sampling off |
| Sparse upload decoder | 11 cases, 565,112 assertions passed under ASan/UBSan | Actual decoder, including bounded sparse rectangle uploads |
| Zen 2 tuning | Native build and package checks passed; generated configuration includes `-march=znver2 -mtune=znver2` | Host application compilation; not guest AArch64 recompilation |
| ThinLTO | Native link, required checks, and packaging passed; core/frontend compile commands include `-flto=thin` | Existing Dynarmic-only ThinLTO remains in the baseline; this adds application/core IPO |
| Bundled PGO profile | LLVM 18 and LLVM 21 reject its instrumentation format version | Cannot use the supplied LLVM 23 profile directly in the current port |
| Existing installed build | MKW stopped after 303 seconds with `VK_ERROR_OUT_OF_DEVICE_MEMORY` | Observed before deploying Selevet; installed executable matched the other session's upload8 receipt |
| First Selevet console run | Rendered title at about 4–5 FPS; `std::bad_alloc` on GPU thread after 394 seconds | No Free Roam measurement; no gameplay performance improvement established |

The mapping benchmark builds `headless/selevet-mapping-check.cpp` with a
reference header from the original core. It uses a fixed random seed and checks
both emitted updates and final effective ranges. The measured ratio depends on
mapping count and workload. It does not predict a frame-rate multiplier.

The current native build uses Clang 18. A supplied profile must pass
`llvm-profdata-18 show` before the build configures. Training a profile for the
actual PS5 binary remains preferable to transferring Windows training data;
compiler version, function identity, platform, and scene coverage all matter.

## Game-code investigation

`tools/nso-analysis-image.py` reads the user's local NSO files, validates their
decompressed segment hashes, and produces private AArch64 ELF analysis carriers
with separate executable, read-only, writable, and BSS sections. It recovers the
NSO dynamic symbol extents, including 28,125 SDK entries. The carrier is derived
analysis input, not the game's original ELF or a live relocated image.

LLVM disassembly identifies SDK module offsets `0x16eda0` and `0x16edf0` as the
exported address-wait and address-signal wrappers. Ghidra 12.1.4 decompiled the
signal wrapper by its exported symbol. Its assembly agrees with LLVM after
subtracting Ghidra's `0x100000` image base: analysis address `0x26edf0` corresponds
to NSO offset `0x16edf0`. The wrapper supplies signal type 0, value 0, and wake
count 1 to the stub at NSO offset `0x1a4588`, which issues SVC `0x35`; an error
takes the SDK's failure path. This supports preserving real wake and error
semantics rather than replacing the call with an unconditional success.

The address-wait wrapper passes arbitration type **3** and the full `x1` value
through `x2` to SVC `0x34`. The current Eglantine core accepts only types 0–2 and
narrows the comparison value to `s32`. Atmosphère's
[SVC type definitions](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libvapours/include/vapours/svc/svc_types_common.hpp)
identify type 3 as `WaitIfEqual64`; its
[arbiter implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_address_arbiter.cpp)
uses an `int64_t` value and requires 8-byte alignment for that type. This exposes
a specific missing ABI path to investigate: enum validation, argument width,
alignment, and the kernel's 64-bit comparison under scheduler synchronization.
Static presence in the SDK does not establish that MKW exercises the wrapper
during the measured scene, or that fixing it would improve FPS. Runtime call
coverage and a full-width comparison regression are needed before that claim.

Full main-module auto-analysis exceeded its 20-minute startup deadline. A
bounded analysis instead used the module's `.eh_frame_hdr` table: the FDE at
NSO image offset `0x3255978` gives the function range
`[0x1f52a0, 0x1f6ad0)`, containing the previously observed run-exit PC
`0x1f5b20`. Ghidra analyzed the complete 6,192-byte range and produced assembly
and pseudocode. The recovered body performs repeated floating-point vector
calculations, validity/bounds checks, and calls through external functions.
Its calls to main offsets `0x2bade50` and `0x2bade60` resolve through PLT/GOT
relocations to `sinf` and `cosf`. A spatial-query role is a hypothesis; the
original function name and external callees are not recovered by the fragment.

The fragment omits read-only data and reports 15 unresolved calls. It therefore
does not establish the routine's complete game-level purpose or current scene
cost. A run-exit PC is not an instruction-time profile. Emulator compiler PGO
and CPU tuning also do not directly recompile this AArch64 routine: execution
passes through the runtime guest JIT and its host feature selection.

Game bytes, derived ELF files, decompilation, and shader caches stay outside
tracked source. No game assets are needed to build the public port.

## Memory and synchronization priorities

The installed pre-Selevet executable's SHA256 was
`f0abf4ebc249abd79c434b0589b66ae57ef6a4a26f082ce0f0469deb87cec71e`, matching
the other session's upload8 deployment receipt. Its crash report at console time
2026-11-02 09:24:43 reported MKW, Vulkan, resolution 1×, and a largest free block
of 12 MiB out of 12,224 MiB. A fresh viewed screenshot showed the launcher and
the corresponding crash notification, not running gameplay.

Symbolization with upload8's matched ELF places candidate stack entries in
`MemoryAllocator::CreateBuffer`, `StagingBufferPool::Request`,
`TextureCacheRuntime::UploadStagingBuffer`, and `RefreshSparseTiles`. These are
stack-scan candidates rather than a verified unwind. The sparse-refresh path
requests staging storage from `UnswizzledRectsBytes`; investigating its maximum
request and lifetime is a concrete next step. This does not yet prove a leak or
establish the size of the failed allocation.

The other local session subsequently added 4 MiB streaming for sparse rectangle
uploads in upload10/upload11. That change was merged as a common baseline for
the Selevet tests, in addition to the earlier bounded layer and whole-image
uploads. Its claimed console improvement still needs attributable run evidence.
Inherited per-run JIT timing and native sampling probes are now behind
`EDEN_MKW_DIAGNOSTICS`, off by default. Comparisons must hold that option fixed;
removing diagnostic overhead is not itself evidence of a Selevet speedup.

The full development profiler is not qualified for all six Switch 2 CPU cores.
Its named worker list currently covers CPU0–3. Snapshot progress records are
limited to those CPU workers rather than mislabeling GPU/timing workers as
CPU4/5 after the JIT counter arrays were expanded.

## First attributable Selevet console run

The candidate eboot SHA256 is
`5a8ff195572515722738bd283253baff30bff4a01ddd72bf66b413d6209b3e70`.
Its archived ELF SHA256 is
`4f79ca93e2d6694a77f703502832441b46059ee81ae702c93dc28fa9a59cefec`.
All 65 other package files matched the installed files before deployment. The
fresh process was PID 530, with firmware SDK version `0x12200005`. Viewed live
captures showed the MKW 1.8.0 title and the subsequent camera transition.

The automation paused for visual review before further input; it never sent
the Free Roam command or started its planned gameplay interval. The candidate
later stopped with `std::bad_alloc`, with a largest free block of 24 MiB. The
matched ELF identifies stack-scan candidates in command-memory scratch reading
and `DmaPusher::Step`. This does not establish malformed commands or implicate
the mapping-table optimization by itself.

The runner's initial rollback was rejected before mutation because its backup
directory was not named `PPSA98515`, as the deployment client requires. The
backup was preserved and the directory handling was corrected for subsequent
runs. Another session then acquired the lease for its own deployment; its run
was preserved, and the rollback failure was posted to the shared mailbox.

That prior session's upload12 also includes smaller guarded fiber stacks and a
lower whole-image streaming threshold, beyond the upload11 sparse fix merged
here. Comparisons must put those memory changes on a common baseline before
attributing any difference to Selevet. Zen 2 and whole-application ThinLTO builds and packages now pass. Console
loading failures prevent a controlled gameplay comparison; no gain is attributed
to either compiler option.

Compiler tuning cannot resolve an allocation failure by itself. A sustained
60 FPS assessment needs bounded memory use and the cost of CPU translation,
GPU submission, synchronization/readback, and texture streaming measured in the
same rendered scene. Unsafe semaphore/command shortcuts also need correctness
checks; apparent FPS with stale work is not a valid result.

## Compiler and command-drain console trials

Each trial verified the candidate eboot hash and all 65 other installed package
files, recorded a fresh process, preserved same-boot logs, and reviewed a live
capture. Builds used Vulkan/RADV at the recorded common settings. These are
startup/correctness results, not a gameplay tuning benchmark.

| Variant | Fresh PID | Visible result | Bounded frame observation |
| --- | --- | --- | --- |
| Custom / LTO off, fork default | 530 | MKW 1.8.0 title and camera transition; later GPU-thread `std::bad_alloc` at 394 s | Title HUD around 4–5 FPS; no controlled gameplay interval |
| Same eboot, `EDEN_NX2_COMMAND_DRAIN=1` | 535 | Loading map remained visible | Total held at 455; 0 new frames over 20.004 s |
| Zen 2 / LTO off, fork default | 537 | Loading map remained visible | Total held at 501; 0 new frames over 20.004 s |
| Custom / ThinLTO, fork default | 540 | App startup overlay at 98%, “Starting the game” | 0 presented game frames across 20.019 s |

The command-drain switch did not demonstrate a usable correctness workaround.
The Zen 2 and ThinLTO trials did not qualify for same-scene gameplay timing.
A single failed startup also does not isolate compiler code generation as the
cause: runtime queue/memory state and remaining port adaptations need controlled
repeats after stability work. Full identities, flags, and interval samples are
in [`results.json`](../experiments/selevet-mkw/results.json).

The later runner corrected both rollback handling and log collection: backup
apps use the required `PPSA98515` directory, restoration checks the current
remote hash, and a watcher preserves logs only when their boot markers match.
The drain and Zen 2 trials restored the preceding executable and DEV settings
with verified deployment receipts. The ThinLTO trial was stopped after its bounded no-frame observation. An
interrupted launcher cut off its original cleanup; a fresh lease and executable
hash guard then restored the prior executable/settings successfully. The shared
helper now uses `Popen.wait` to let child cleanup finish on terminal SIGINT; a
loopback process test confirmed the lease remains held throughout cleanup.
The initial failed rollback remains part of the evidence above; it is not
retroactively counted as successful.

## Reproduce the optional port

The normal PR checkout keeps the existing core. The complete experiment is
saved as [`experiments/selevet-mkw/ps5-port.patch`](../experiments/selevet-mkw/ps5-port.patch).
It includes the fork URL/gitlink, PS5 adaptation patch, and the captured local
MKW compatibility baseline. Apply it in a separate clean checkout:

```sh
git apply --index experiments/selevet-mkw/ps5-port.patch
git submodule sync -- Eglantine
git submodule update --init -- Eglantine
bash tools/apply-eglantine-patches.sh
```

This reconstruction was checked from a clean checkout; the nested patch
reproduced all 131 changed/new core files byte for byte, including upstream
CRLF line endings. The optional snapshot retains temporary MKW launch and
loading probes from the captured baseline, including a forwarded-ROM adapter
that targets MKW. It is investigation source, **not a release candidate**.
It also changes the PS5 CPU page-table budget to 128 MiB. Do not interpret
successful host checks as qualification of those experimental choices.

Use the environment from the PS5 build procedure and failure-chain source
synchronization with `tools/build-headless-native.sh --graphics`. Set one of:

| Variant | Additional environment |
| --- | --- |
| Baseline | `EDEN_BUILD_PRESET=custom EDEN_NATIVE_LTO=OFF` |
| Zen 2 | `EDEN_BUILD_PRESET=zen2 EDEN_NATIVE_LTO=OFF` |
| ThinLTO | `EDEN_BUILD_PRESET=custom EDEN_NATIVE_LTO=ON` |
| Matching PGO profile | `EDEN_PGO_PROFILE=/absolute/profile.profdata` |

Keep `EDEN_DEV_PROFILE=OFF`, `EDEN_MKW_DIAGNOSTICS=OFF`, the same ROM ID,
renderer, resolution, save, scene, and DEV settings. Archive the matched ELF
before packaging another variant. The ThinLTO lock check inspects the actual
`Core::DynarmicExclusiveMonitor::ClearExclusive` caller when IPO removes the
standalone Dynarmic symbol; exchange acquisition/release, pause, full fence,
and absence of runtime calls remain required.

The mapping differential harness takes `REFERENCE_MAPPING_HEADER` naming the
header from original core `3140648a668aaebf87bae2dea17512fdca125ba0`; compile
with the fork's `src` and `externals` include paths. The fixed-seed correctness
and timing workloads are in the public harness; timings must be remeasured on
the reader's host.

## Work toward 60 FPS

60 FPS permits 16.67 ms per presented game frame. A title running near 4–5 FPS
has roughly 200–250 ms between frames; the current evidence does not support
compiler flags closing that gap. The immediate priorities are:

1. **Make memory use bounded and execution stable.** Track live staging,
   readback, texture, JIT, fiber, and host-heap backing separately. Preserve
   failing request sizes and lifetimes. The 4 MiB sparse upload chunks alone
   did not establish a fix; the first candidate still exhausted memory.
2. **Resolve loading/queue correctness before timing gameplay.** Investigate
   ordered readbacks, fence publication, command-page visibility, and the
   loading stalls below. Preserve wake/error semantics and device-virtual
   address meaning. The conservative command-drain switch is not a demonstrated
   workaround.
3. **Measure CPU/JIT and GPU costs in one sustained rendered scene.** Qualify
   profiler arrays and named workers for all six Switch 2 cores first. Separate
   guest execution, JIT compilation/cache clears, GPU submission, fence waits,
   and texture decoding. A sampled run-exit PC or fast mapping microbenchmark
   cannot identify the dominant frame cost.
4. **Then repeat isolated mapping/Zen 2/ThinLTO comparisons.** Keep a common
   memory baseline and use repeated warm runs with frame-time percentiles,
   not only average FPS. Test mapping changes with captured workloads and
   rendering checks, not only table equivalence.
5. **Train PGO with the PS5 toolchain and representative work.** Build an
   instrumented binary with the same compiler, arrange usable profile output
   and runtime support on PS5, merge using its matching `llvm-profdata`, then
   compare profile-use against an otherwise identical build. Include loading,
   sustained Free Roam/racing, shader compilation, and synchronization. No PS5
   training profile was collected in this experiment. The Windows LLVM 23
   profile cannot be consumed by the qualified LLVM 18 build.

The statically identified `WaitIfEqual64` ABI gap is a separate correctness
candidate: prove runtime use, implement full-width/alignment behavior under
the kernel scheduler, and add a comparison/wake regression before timing it.
It is not evidence that this call caused the observed stalls.

## Console coordination

`tools/console-lease.py run --wait -- COMMAND` holds an OS advisory lock for a
complete experiment and queues behind the active owner. All local sessions
must adopt the shared lock; older scripts can bypass it. `status` tests the
actual lock, and `note TEXT` appends to the shared findings mailbox. Findings
and the helper were also shared with the original checkout. The lock files
must not be deleted. See [CONSOLE_COORDINATION.md](../CONSOLE_COORDINATION.md).

AI assistance: Codex performed the port adaptation, experiments, and report
drafting. No human testing or sign-off is claimed.
