# Mario Kart World: where the console's memory goes

Status, 2026-11-05: every title-screen run at 0.5x ends within 120–221 s. All three crash types
come from one cause: the title's single 12 GiB direct-memory pool runs out. The pool holds guest
RAM, the driver's GPU memory, Eden's heap, and everything else at once. The crash types are:

- `VK_ERROR_OUT_OF_DEVICE_MEMORY`
- `VK_ERROR_DEVICE_LOST`
- `SIGABRT` on a guest CPU thread

Measured with `tools/mkw-bench-console.py` (docs/MKW-BENCHMARK.md).

## Accounting at the crash

From a diagnostics build (`EDEN_MKW_DIAGNOSTICS=ON`, `MKW_MEMORY` every 5 s) with the driver
report on (`RADV_PS5_MEMORY_REPORT=1`):

| Seconds | Guest RAM | Eden heap (+ large) | Eden tables | Driver | Largest free |
|---|---|---|---|---|---|
| 25 | 1.74 GB | 1.04 + 0.17 GB | 0.22 GB | (loading) | 8.09 GB |
| 55 | 4.28 GB | 1.28 + 0.42 GB | 0.24 GB | ~3.9 GB | 1.59 GB |
| 85 | 4.60 GB | 1.38 + 0.42 GB | 0.24 GB | ~4.0 GB | 0.63 GB |
| 145 | 4.88 GB | 1.51 + 0.42 GB | 0.24 GB | 4.22 GB | **4 MiB** |

About 1 GB more is JIT code caches, thread stacks and libraries. The driver's 4.22 GB breaks down
by RADV priority as follows:

- 3.87 GB application memory (VMA)
- 0.26 GB descriptor and upload buffers. This grows through the whole run: 31 → 256 MiB, 822
  objects.
- 0.09 GB shaders

A VMA dump at 80 s shows 3.79 GB of blocks holding 3.26 GB of allocations:

- 0.95 GB render targets, including the game's 8192×8192×68 virtual-texture arrays
- 0.58 GB sampled textures
- 0.60 GB sparse bindings
- 0.53 GB buffers
- 0.53 GB empty space inside blocks

## What does not help

- **The texture collector.** It already measures the pool's largest free block
  (`graphics_usage_from_pool`) and evicts at its hardest when the pool runs short. At the crash
  it scans and finds nothing evictable: the images are in use.
  - Lower limits (`EDEN_NX2_TC_EXPECTED_MIB=4096`, `EDEN_NX2_TC_CRITICAL_MIB=4608`): crash at
    193 s, against 190 s with the defaults.
  - Much lower limits (3584/4096): crash at 133 s and 18 FPS, from re-upload churn.
- **ASTC→BC3.** Switch 2 ASTC is already re-compressed to BC3 on this pool
  (`UseBc3ForSwitch2OnSmallCards`).
- **ProsperoEden's guest-memory states (Zero/Shown).** They save CPU page tables, not memory.
  Eglantine's guest service is already newer here: it keeps a 512 MiB reserve.

## Dense GPU-only regions backed lazily (ProsperoEden 0029/0031)

Core patch `0022-dense-lazy-gpu-regions.patch`. nx2:gpu 214 GPU-only regions of 16 MiB or more
get memory as the GPU writes them, like sparse regions already did.
`EDEN_NX2_DENSE_LAZY_MIB=<n>` moves the threshold, and `0` backs them whole again.

Measured on 2026-11-07 (MKW 1.8.0, title, 1x, interleaved, `Service:Info` log in a separate run):

| | Lazy (default) | Whole (`=0`) |
|---|---|---|
| Regions backed whole | 75 small ones, 31 MiB | 82, **8,830 MiB** |
| Guest kernel allocation failures | none | one 1,453 MiB region got no backing and stayed unmapped |
| FPS (2 runs of 240 s) | 20.5 / 23.4 | 26.2 / 18.6 |
| Out of device memory at | 163 / 173 s | 171 / 113 s |
| Pool free at 150 s | ~550 MiB | ~470 MiB |

**It does not fix the crash on this core.** Unlike ProsperoEden, Eglantine never clears a
GPU-only backing ("touching 300 MB would commit it on the host"), and guest memory takes host
memory only when written. A dense region backed whole already costs the console little more
than the lazy one. What the patch does change is the guest kernel side. Backed whole, the
regions used 8.8 GB of the guest kernel's system pool, and the largest one could not be
allocated: the GPU's writes to that region had nowhere to go. Backed lazily, every region
gets a mapping.

The crash's cause is therefore still the host-side growth listed below, not the GPU-only regions.

## The driver's own heap figure counts memory twice

The PS5 winsys reports its whole allocation total for both of its heaps. On MKW, VMA's
`heapUsage` was 7.8 GB with 3.5 GB of VMA blocks, while the per-priority report shows the
driver's true total near 4 GB. Pool-based usage is the default, so this does not change the
collector's decisions. Read the per-priority report rather than `VMA heap N: usage`.

## Where the remaining savings are

1. **Guest RAM: 4.9 GB, still growing at the crash.** The game maps 9.2 GB of its 12 GiB
   layout. Only written 2 MiB slots are held. The 512 MiB guest reserve sits idle while
   graphics allocations fail.
2. **Driver descriptor and upload buffers** grow without bound (≈ +1.5 MiB/s). Check whether
   descriptor pools or command-buffer upload buffers are ever trimmed.
3. **VMA internal fragmentation** (~0.5 GB) and the 0.6 GB of sparse bindings.
4. **Eden's heap** (1.9 GB) grows ~2 MiB/s through the title.

## Tools

- `tools/patch-radv-memory-report.py`: applied to the pinned RADV at driver build time. It
  keeps live bytes and objects per RADV priority. With `RADV_PS5_MEMORY_REPORT=1` in `env.txt`,
  it prints `RADV_PS5_MEMORY pool_mib=… available_mib=… driver_mib=… pN=MiB/count` to stderr at
  most every 10 s. Without the variable it only maintains the counters.
- `EDEN_MKW_DIAGNOSTICS=ON` builds print `MKW_MEMORY` (guest, heap, tables, largest free) every
  5 s.
