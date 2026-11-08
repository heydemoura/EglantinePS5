# Mario Kart World: where the 12 GiB pool goes, and what is never given back

Status: **findings, and the first fixes** (2026-11-08; see "Fixed" at the end). The crash at the end of every long MKW session at 1x is the
pool running out (`VK_ERROR_OUT_OF_DEVICE_MEMORY`, or a code-2 SIGSEGV with the largest free
block at 1 MiB). This file records what holds the pool over a session and which parts never come
back, measured on the console with the diagnostic branch `diag/mkw-leak`.

## How it was measured

- Build: `main` at e573a23 (core patches 0001–0050), scripted input, plus the diagnostic branch:
  - `EDEN_MEMORY_PARTS` every 10 s: the driver's real buffer bytes (`radv_ps5_allocated_bytes`)
    and the guest memory the service holds.
  - `EDEN_CRAM_COLD` (`EDEN_CRAM_COLD_REPORT=1`) now also counts held 2 MiB slots that look
    all-zero.
- Runs: `tools/mkw-bench-console.py --events` from the title, L+R, menus, Grand Prix, into the race
  (1x and 0.5x). The texture cache's 600-frame summaries and the allocator's per-heap lines were
  kept with `EDEN_LOG_FILTER=*:Warning Render.Vulkan:Info HW.GPU:Info`.
- Bench folders on the dev machine: `.local/bench/b82-leak-1x`, `b83-leak-05x`,
  `b84-leakdiag-05x`, plus the earlier race runs `b69-mme-race05`.

## What holds the pool

0.5x, diagnostic build (MiB; t counted from the first sample after boot):

| t | Pool largest free | Game memory held | Driver buffers (real) | Eden heap | Game memory unwritten for 60 s |
|---|---|---|---|---|---|
| +23 s | 9,600 | 1,553 | 248 | 1,088 | 0 |
| +33 s | 6,718 | 2,859 | 1,628 | 1,216 | – |
| +43 s | 4,540 | 3,618 | 2,819 | 1,388 | 8 |
| +53 s | 2,984 | 4,205 | 3,662 | 1,452 | – |
| +63 s | 1,939 | 4,314 | 4,470 | 1,612 | 658 |
| +73 s | 1,872 | 4,560 | 3,432 | 1,644 | – |

1x, the same title → race path (20 s samples):

| Pool largest free | Game memory held | Game memory mapped by its kernel | Eden heap |
|---|---|---|---|
| 9.7 GB | 1.6 GB | 8.6 GB | 1.0 GB |
| 4.6 GB | 3.6 GB | 9.1 GB | 1.3 GB |
| 2.4 GB | 4.3 GB | 9.2 GB | 1.4 GB |
| 1.8 GB (title ends) | 5.0 GB | 9.4 GB | 1.5 GB |
| 1.2 GB (race loading) | **6.1 GB** | 9.6 GB | 1.5 GB |

## Findings

### 1. The game's memory: written once, held for good (the largest and the one that only grows)

- MKW's kernel maps ~9.5 GB for the game at boot. Eden backs a 2 MiB slot with real memory the first
  time the game writes it (`src/guest_memory_service.inc`). It gives a slot back only when the
  game returns that memory to its kernel (`returns_free`: 532 slots, ~1 GB, in a whole session).
- MKW frees and reuses memory inside its own heap, which the kernel (and so Eden) never sees.
  When the title scene ends, its data stays held, and the race's data goes into slots the game
  had not written before. Game memory held grows **1.5 → 6.1 GB** from boot through the race
  start in every run, and is the one figure that never goes down (only the LZ4 packer shrinks it).
  In the one session that lasted (b69, 420 s) it levelled off at ~5.5 GB.
- It is real data, not cleared memory: all-zero slots stayed at 4–8 MiB, and sampled cold slots
  compress to 0.29.
- A good part of it goes cold: 658 MiB unwritten for 60 s while still on the title, and the
  packer compressed 1,364 MiB unwritten for 60 s or more once the race was loading (b82).

On the console this is real hardware memory for the game. On the PS5 it shares one pool with
Eden's copies of the same textures (finding 2), Eden's heap and the driver, and that sum does not
fit.

### 2. Freed GPU memory comes back scattered: the largest free block does not recover

- When the title scene goes, the texture cache deletes ~2,700 images and the driver frees
  **1,038 MiB** (+63 → +73 s). The pool's largest free block **does not grow** (1,939 → 1,872 MiB).
- The driver's buffers come from 2 MiB pieces once the pool has no long run left
  (`tools/patch-radv-pieces.py`). Freeing them returns 2 MiB holes between the guest slots and
  heap pieces, not a block a later large allocation can use. The overlay's M figure (the largest
  free block) stays down after the title for this reason.

### 3. The texture collector reads twice the driver's memory, and churns

- The PS5 driver reports its buffers as both `allocated_vram` and `allocated_vram_vis`, and RADV's
  APU budget adds the two (`radv_get_memory_budget_properties`). Eden's
  `Device::GetDeviceMemoryUsage` subtracts only the allocator's unused space
  (`AllocatedUsage`), so the collector's "usage" is about twice the driver's real memory.
- So the collector sits at or above its critical limit (4 GiB) while the driver really holds
  3.2–3.4 GB. In 600 frames during the title's end it created 5,451 images and deleted 5,648
  (3,614 of them costly re-decodes). On the title before that it logged `scanned=0` for long
  stretches: every image had been used within the aggressive age, so it had nothing to delete,
  while the double-counted figure kept it in aggressive mode.

### 4. Eden's heap grows and never gives back

- 736 MiB → 1,772 MiB over a session (in use 481 → 1,267 MiB), "0 MiB given back" throughout.
  The heap-site report covers only ~500 MiB of it, so the growing allocations are not
  identified yet.

### Not the cause

- The driver's own memory: it is almost all the allocator's blocks (3,175 MiB of blocks against
  3,432 MiB of driver buffers), and it is freed when images are deleted.
- The texture cache keeping title textures: it deletes them (941 → 100 images when the title ends).

## Also seen in these runs (separate problems)

- **Freeze:** twice (b74 run 1 at ~75 s, b84 at ~100 s) MKW stopped presenting and every log
  stopped, with no crash report and no "GPU has not reached submission" line. Both builds were
  `main` after #80.
- **GPU hang without the transfer split:** b82 (1x, race loading) logged "the GPU has not reached
  submission" and device lost at 171 s on a build without `patch-radv-transfer-split.py`. So the
  split was not the only cause of the hangs PR #77 attributed to it.

## What would fix it, in order of payoff

1. **Give back the game's freed memory.** Release slots the game no longer uses without waiting for
   its kernel:
   - slots covered by GPU mappings the game unmaps (`Nx2GpuMemoryRegistry::Unmap`), whose data the
     game must write again before the GPU reads it;
   - cold slots under pressure: pack them, as now, or drop them when they are covered by texture
     cache images that hold the same data.
2. **Count the driver once.** Make `GetDeviceMemoryUsage` subtract the duplicated
   `allocated_vram_vis`, or read `radv_ps5_allocated_bytes` as `EDEN_MEMORY_PARTS` does, so the
   collector stops churning at a false critical.
3. **Keep the pool unfragmented.** Take the driver's big blocks before the guest's 2 MiB slots
   scatter the pool, or let the guest memory service hand 2 MiB pieces back to the driver in
   runs.
4. **Find the heap growth.** Extend the heap-site report to cover all allocations.

## Fixed (branch fix/mkw-memory-leaks)

### The race-load "out of memory" was one impossible index buffer (core patch 0051)

What ended 1x sessions as the race loaded was not the gradual growth above but a single request.
Switch 2 titles leave the index limit at 0, and the count/first registers sometimes hold values no
draw uses (1.4–2.9 billion indices). The 32-bit span arithmetic wrapped to a 1.3–2.9 GB "index
buffer"; the buffer cache merged up to 101 buffers into it, RADV built it from up to 1,372 2 MiB
pieces, and a staging buffer of the same size was asked for at once ("recover asked=2768MiB"). The
pool ran out on the spot, even with 2.4 GB free.

Core patch 0051 works out the span in 64 bits, treats an index limit below the start as no
bound, caps an index binding at 128 MiB, and drops an indexed draw whose indices reach past the
cap. Capping alone was not enough: run on the GPU, a 559 million index draw kept the queue past
the driver's 10 s limit and the device was lost.

### Packing is off by default (`EDEN_CRAM_PACK=1` turns it on)

Every GPU hang ("the GPU has not reached submission N in 10 s", then `VK_ERROR_DEVICE_LOST`, and
several times the console dropping off the network) came in a session that packed, seconds after a
packing burst. The impossible draws above came only in sessions that packed (none in the
packing-off runs). Some path reads the game's memory without faulting on a packed slot, so the
emulated GPU read garbage. Keeping packing's freed blocks for unpacking
(`EDEN_CRAM_UNPACK_KEEP_MIB`, default 256) removed the waits on unpacks (3.5 GB packed, 206
unpacks, no hang in 4 runs), but a later run still hung after a 506 MiB pass with 3 unpacks. So
packing stays off until it skips memory the GPU path uses. The keep and the tunables
`EDEN_CRAM_PACK_BELOW_MIB` / `EDEN_CRAM_PACK_COLD_S` stay for when it is turned on.

### Tried and dropped: giving back destroyed GPU memory

Returning the whole 2 MiB slots of GPU memory the game destroys (nx2:gpu 102) freed only ~219 MiB
a session, and the game still reads some of that memory. With it, sessions died of guest faults
("ActorLoading", "ARes Loading N jumped to an unmapped address": 1 of 2 runs, and 2 of 2 in
another set); without it, 0 of 2.

## Result

MKW 1x, title into a race, on the final build (packing off, patch 0051):

| Runs | Length | Result |
|---|---|---|
| 4 | 240 s | all ran the full 240 s (no out-of-memory, no GPU hang; the guest fault only in the runs that gave back destroyed memory) |
| 2 | 330 s | ended at 241 s and 292 s, the pool exhausted (largest free block 2–16 MiB, game memory 6.0–6.2 GB, Eden's heap 1.7 GB) |

Before: every 1x session that reached the race load ended within 131–211 s, by out-of-memory
(the impossible buffer), a GPU hang (packing), or a guest fault.

## Still open

- **The game's memory and Eden's copies still fill the pool.** At 240 s the largest free block is
  12–337 MiB, with 5.7–6.2 GB of game memory held. Making packing safe is the way to give that back.
- Findings 2–4 above (scattered free memory, the collector's double count, heap growth) are
  unchanged.

## Packing safety (branch diag/pack-safety)

MKW 1x, early packing (`EDEN_CRAM_PACK=1 EDEN_CRAM_PACK_BELOW_MIB=4096 EDEN_CRAM_PACK_COLD_S=20`).

**Packing itself is correct.**
- `EDEN_CRAM_PACK_KEEP=1` keeps a copy of every packed slot and compares it on unpack: 1,627
  unpacks compared, 0 differed.
- The shared zero block was never written (`zero_block_dirty=0`).
- Garbage persists with the GPU split off (`EDEN_NX2_SPLIT=0`).

**GPU threads no longer unpack.**
- `EDEN_CRAM_UNPACKERS` counts unpacks by thread name.
- Before: draws past the index cap appeared only in runs where the command reader ("GPU") unpacked.
- Skipping slots with pages in the GPU caches cut that down but did not stop it: a slot can be
  reached before it is cached.
- Core patch 0053 now reports every page DeviceMemoryManager maps for the GPU. Slots holding
  any such page are never packed.
- With 0053 (b107): no unpack by a GPU thread, no GPU hang, and 354–518 MiB packed (it was
  0.4–1.9 GB when everything cold was packable).

**The garbage draws are not packing's.**
- Even with no GPU unpack, these runs still showed the race-load event:
  - 16 indexed draws of ~1.1–1.4 billion indices, dropped by 0051.
  - The draw macro at 0x1e6 failing with "instruction limit exceeded" every ~0.1 s.
  - Then a slideshow (0.1–0.7 fps), corrupted textures, or a device lost.
- One count was 1065353216 = 0x3F800000, the float 1.0: the macro is fed a float where the game
  puts a count.
- The same event appears with packing off, just less often: b94 1 of 3 runs, b93 1 of 2.
- Ruled out as its cause:

| Run | Change | Result |
|---|---|---|
| b103 | GPU split off | 2/2 runs with the event |
| b108 | MME read-ahead off (`EDEN_NX2_DMA_READ_AHEAD=0`) | 2/3 runs with the event |
| b109 | native MME macros off (`EDEN_NX2_MME_NATIVE=0`) | every run with the event; prime device lost |

**Next:**
- Find where the macro's parameters come from at that point: the command list words around the
  0x1e6 calls, compared between a good run and a bad one.
- Packing is safe to turn back on only after that.
