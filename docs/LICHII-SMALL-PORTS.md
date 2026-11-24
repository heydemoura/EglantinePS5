# Two small things from PS5_Lichii (November 2026)

Read from PS5_Lichii at commit 5a424f7 (`docs/ps5/texture-cache.md`, `docs/ps5/levers.md`,
`docs/ps5/gpu-thread.md`, `ps5/src/fast_mem.cpp`). Neither was measured on its own; the build with both ran on the console (`docs/LICHII-MEMORY-PLAN.md`, "The console runs").

## The driver's memory counted twice

Lichii: `radv_query_heap_info` filled `allocated_vram` and `allocated_vram_vis` with the same bytes
and a device without dedicated VRAM adds them, so `VK_EXT_memory_budget` reported twice the
driver's memory and its texture cache collected as if the heap were nearly full.

Our driver (PS5_Mesa 7b59ef27, `tools/deps.json`) has the same fault:

- `radv_physical_device.c`, `radv_query_heap_info`: `heap_info->allocated_vram = driver;` and
  `heap_info->allocated_vram_vis = driver;`.
- `radv_ps5_winsys.c`: `info->has_dedicated_vram = false;`, so `radv_get_memory_budget_properties`
  takes `vram_vis_internal_usage = heap_info.allocated_vram_vis + heap_info.allocated_vram` and
  reports it as the device-local heap's `heapUsage`.

But nothing reads that number in a default build, because the port already went around it:

- VMA is made without `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` (`vulkan_device.cpp`: "The
  driver's budget is not asked for: its count is double"). Its "driver X of Y MiB" in the
  `Memory trace: heaps` line is its own estimate: X is the bytes of its blocks and Y is 80% of the
  heap's size. `driver 4751 of 7168 MiB, VMA 4595/4751 MiB` shows exactly that (4751 twice, and
  7168 + 3584 is 80% of a 13,440 MiB pool split 2:1).
- The caches' "device usage" is the budget less the pool's largest free block
  (`Device::GetDeviceMemoryUsage`, `headless/performance.h` `graphics_memory_free`).
  `device usage 6143 MiB` is a 6 GiB budget with no free block left (the probe answers 1 byte),
  not a count of the driver's memory; a double count of 5.6 GB would have read over 11 GB.

So Lichii's gain (texture churn 2,000-2,900 to 660-1,700 images per 600 frames) is not waiting
here, and the driver is left as it is.

Lichii's other driver lever is "budget and available memory count the whole shared pool, in 2 MiB
pieces". Here the driver's "system usage" is the pool less `sceKernelAvailableDirectMemorySize`'s
answer, which is the largest free run, and `RefreshFreeMemory` (`headless/performance.cpp`) gave
the caches the same figure. Since `tools/patch-radv-pieces.py` the driver can build a buffer from
free 2 MiB pieces, so with a fragmented pool that understated what graphics can still get (Mario
Kart World once had ~650 MiB free in pieces and a 24 MiB largest run), and the caches collected
for nothing. Now the guest memory service's keeper, which counts the pool's free pieces every
pass anyway, keeps its last count (`eden_pool_free_pieces`, not older than half a second), and
`RefreshFreeMemory` gives the caches the larger of the two. `EDEN_POOL_FREE_PIECES=0` gives them
the largest run again. Not measured on its own.

## Our own memcmp

The executable imports `memcmp` from the console's `libSceLibcInternal` (the SDK's `libc.a` has
none; `sce_module/libc.prx` is a shim the loader wants, not what the import binds to). A build of
2026-11-23 reaches it from 1,800 places. Lichii measured that function at a byte at a time: 14 ns
for 8 bytes, 218 ns for 2 KiB, against 2.9 and 38 ns for a 32-byte AVX2 loop, and 2.6% of its GPU
back thread in a race (1.3% after; 28.2 to about 27.4 ms a frame).

`src/fast_memcmp.c` is that function, in C. In the PS5 build (`PS5_NATIVE`) it also carries the
names `memcmp` and `bcmp`, hidden: a definition in the executable's own objects wins over a shared
library's, so every caller linked in takes it, the Vulkan driver's included, and the import goes
away. It asks for AVX2 itself (`target("avx2")`), since the executable is built for generic x86-64
unless `EDEN_CPU_PRESET=zen2`. Host builds do not compile the file.

`python3 -B tools/check-memcmp.py` builds it with the system compiler and compares it with the
host's memcmp and a byte loop (18.4 million comparisons: every length to 4096, every pair of
alignments, a differing byte at every position up to 96 bytes, both signs, buffers ending at an
unreadable page), checks that a program linked with the `PS5_NATIVE` part calls it as `memcmp`,
and that the object clang-18 makes for the console defines `memcmp` and `bcmp` hidden and calls
nothing. It is not run by the build scripts.

Not measured here: the time on the console, and the frame time with it.
