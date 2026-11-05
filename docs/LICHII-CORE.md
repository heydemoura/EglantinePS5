# Lichii core update

The `Eglantine` submodule (`https://github.com/zortos293/Eglantine`, branch `nx2`) is the source of
the [Lichii](https://github.com/zortos293/Eglantine/releases) emulator; the repository keeps its
historical name. This update moves it from `3140648a66` (2026-11-01) to `99ac3d5955` (2026-11-05):
127 merges, with Lichii's boot and rendering fixes for Kirby Air Riders, The Legend of Zelda:
Ocarina of Time, Star Fox, Yoshi, Fire Emblem: Fortune's Weave, Mario Tennis Fever and others.

On 2026-11-02 Lichii replaced its renderer and synchronization implementation with a refreshed
upstream tree ("Carry our branch history onto the refreshed game-fix tree"). The PS5 patches
written against the earlier implementation were checked one by one against the new code.

## Patches in `tools/eglantine-patches`

| Patch | Now |
|---|---|
| 0001 extracted versioned content | Kept, merged with Lichii's lazy loose RomFS (`LooseRomFS`). |
| 0002 Ampere U16 F2I source modifiers | Kept on top of Lichii's own U16 absolute support: adds the negate bit, keeps its stricter field checks. |
| 0003 audio out manager session lifetime | Kept: Lichii holds the manager in a `shared_ptr` but its sessions did not. |
| 0004 Mario Kart performance and readback | Dropped. It changed the core readback cache, sparse regions, the GPU mapping table and fence handling that Lichii removed in its refresh. Its own review (`MKW-PERFORMANCE-REVIEW.txt`) measured no frame-rate gain. Its acquire-clock change returns as 0018. |
| 0005 gate allocation and transcode dumps | Dropped: it gated diagnostics that 0004 added. |
| 0006 PS5 unwritten page reads and spin waits | Kept: the Switch 2 memory layout switch, the graphics server region for smaller layouts, the semaphore word checked before draining, and the Vulkan worker spin. Dropped: the readback and drain parts Lichii rewrote (`WaitForWriters`/`WaitForSync`, 16 MiB VMA blocks). |
| 0007 PS5 texture cache costs and split diagnostics | Kept: the kernel spin count and the wake-up fields the frontend uses (`physical_core`). Dropped: frames ahead, ASTC opacity and texture cache validation, which Lichii now does itself. |
| 0008 Bananza terrain counters | Dropped: it hooks the native macro engine that Lichii removed; Lichii has its own Donkey Kong Bananza fixes. |
| 0009 software HMMA | Dropped: Lichii emulates HMMA by default. |
| 0010 sparse zero clears | Dropped: in Lichii (`40902b09c7`). |
| 0011 sparse VMA pool | Dropped: Lichii has a separate pool for sparse pages (`Device::SeparatePool::SparsePages`). |
| 0012 collect emptiest blocks | Dropped: in Lichii. |
| 0013 native Ampere HMMA | Dropped: Lichii's compact HMMA path does the same. |
| 0014 pipeline statistics log | Kept. |
| 0015 NIR compute backend | Kept; adapted to `CreateComputePipeline` returning a result struct. |
| 0016 integer add overflow | Kept. |
| 0017 compute qualification statistics | Kept. |
| 0018 satisfied acquire clock | New: from 0004, an already-satisfied semaphore acquire reads no clock. |

## Frontend

- `tools/prepare-vulkan-port.py`: anchors moved to Lichii's renderer (tracing in `PrepareDraw`,
  `DispatchCompute` and the scheduler, `SubmissionPolicy` in `FlushWork`, conditional rendering
  traces, the lazily built compute cache entries). Switch 1 titles keep at most 512 draws per
  submission (the PS5 GPU watchdog); Switch 2 titles already flush every 128.
- Multiplayer: Lichii's room sockets (`ProxySocket`, the BSD descriptor table, `EmulatedPoll`,
  bounded decompression) replace the port's own. The port keeps its packet, room member, LAN
  discovery and interface changes, and the rule that a game keeps the sockets it started with
  (`guest_socket_mode`, now applied to Lichii's `bsd.cpp`).
- The ordered-read capture and loading probes of the Mario Kart investigation are gone with 0004;
  the Vulkan hand-off switch (`mkw-handoff-mode.txt`) remains.
- The GPU thread port accepts Lichii's stop-aware fence wait; the TSC port removes Lichii's
  version of the AMD fallback, so the frequency is measured once.
- Checks of removed code were deleted (fence requirements, readback trackers, Bananza terrain);
  the GPU producer, GPU sync, semaphore acquire, TSC, Vulkan hand-off and multiplayer checks were
  updated to the new core.

## Verified, and not

Checked on a Linux host: the patches apply to `99ac3d5955`; `prepare-vulkan-port.py` runs against
it; every core anchor in `headless/CMakeLists.txt` and its `.cmake` files matches with the console
options on (`PS5_NATIVE`, `EDEN_PS5_OPENGL`, `EDEN_PS5_VULKAN`); the header-only Ampere tests pass
(134 cases); and these checks pass on the generated sources: native GPU thread, GPU producer stop,
GPU sync stop, nvdrv lifetime, load failure, VFS budget, startup performance, TSC fallback, JIT
protection and patch lookup, staging pressure, Vulkan hand-off, transient images, GPU time probe,
semaphore acquire clock, extracted content, multiplayer packets.

Not yet: a full `make` and `make test` (the session that made this update could not download
GitHub archives), and any run on a console. Build it, then qualify the games you play before
relying on it.
