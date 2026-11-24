# BC250: Zelda OoT performance research — mprotect ping-pong (2026-11-15)

Board: AMD BC-250 (8C/16T Zen 2, no cpufreq/cpuidle driver, ~3.5 GHz), Bazzite, kernel
7.2.0-ogc4.1. Game: The Legend of Zelda: Ocarina of Time (0400486024DEC000), Link's room,
1x resolution, NoSharpen, frame generation off. Measurement: `room-measure.py`, 60 s of 1 Hz
guest-present counter samples after the shared 280 s warm-up, one owned game at a time.

## Method

- `perf record -g` (host perf binary from the debug container; `perf_event_paranoid=-1`
  only while profiling) of the running game, JIT symbols via `PERF_BUILDID_DIR=/tmp`.
- `perf stat --per-thread -e syscalls:sys_enter_mprotect,...` per case.
- Emulator logs (`eden_log.txt`): frame/present summaries, error and critical counts.
- GDB/JIT counter work from the earlier room goal (dispatcher contention) was reviewed, not redone.

## Where the time goes (baseline)

GPU back thread (`GPUBack`, pinned to one physical core) is the critical path: ~90 % busy,
IPC ~0.97, while the front GPU thread spends about a third of its time spinning in
`GpuSplit::Drain` waiting for it, and the six guest cores are ~25 % idle (`sched_yield`).

13.6 % of `GPUBack` samples were inside
`DeviceMemoryManager::UpdatePagesCachedCountNoLock` → `RasterizerMarkRegionCached` →
`mprotect`. `perf stat`: **~15.4k mprotect/s on GPUBack and ~19.5k/s more across the guest
cores** (~550 per frame on GPUBack alone, ~9 µs each). The fastmem arena had 18.5k
`memfd:HostMemory` VMAs; every mprotect splits/merges VMAs under the mm write lock and sends
a TLB shootdown to every emulator thread.

Cause: with reactive flushing a cached page has no host access. The game rewrites the same
buffer pages every frame, so each page bounces: guest write → page uncached (mprotect RW,
often replayed on GPUBack through the deferred-write path in `MemoryManager::FlushCaching`)
→ next draw uploads it → page cached again (mprotect NONE).

## Change (`core/memory.cpp`, branch `perf/bc250-lazy-unprotect`)

An uncached page keeps its protection for up to 250 ms. Cached again in that window — the
normal case for per-frame buffers — it needs no syscall in either direction. Pages still
uncached after 250 ms are unprotected in coalesced runs (only if the page table says mapped,
uncached `Memory`). Map, Unmap, ProtectRegion and debug marks drop their pages from the
pending set because they set host protection themselves.

Why it is safe: extra protection only adds traps. A fastmem access that faults is demoted to
the page-table path (dynarmic `do_not_fastmem`), and that path reads the page type, which is
already `Memory` once the page is uncached, so no write notification can be missed. Only the
reactive-flushing configuration (cached = no access) uses it. `EDEN_LAZY_UNPROTECT=0`
restores the old behaviour.

## Results (same binary, env toggle; door view, stationary)

| case | FPS (60 s) | min 1 s |
|---|---|---|
| lazy on (s3-a2) | 33.59 | 32.09 |
| lazy on (s4-a3) | 34.05 | 32.31 |
| lazy on (s4-a4) | 32.92 | 31.34 |
| lazy off (s3-b1) | 27.89 | 24.43 |
| lazy off (s3-b2) | 27.95 | 25.35 |
| znver2 build, no patch (s4-z1) | 27.81 | 24.40 |
| znver2 build, no patch (s4-z2) | 27.98 | 25.46 |
| main build -O3 generic (s2-a1/a2) | 28.05 / 28.16 | 25.36 / 27.34 |

Mean 33.5 vs 27.9 FPS: **+20 %**, and the 1 s minimum stays above 31. mprotect calls:
~36,000/s → ~9/s. Error/critical log counts identical with and without (4 known nonfatal
criticals). Door screenshots render the same.

## Results while walking (A/S/D/W square, 1.5 s each, through the 60 s window)

| case | FPS (60 s) | min 1 s |
|---|---|---|
| lazy on (s5-m3, s6-m6, s6-m8) | 31.31 / 30.15 / 30.32 | 12.71 / 21.42 / 21.42 |
| lazy off (s5-m4, s6-m5, s6-m7) | 26.69 / 26.61 / 24.90 | 18.62 / 1.96 / 19.47 |

Mean 30.6 vs 26.1 FPS (+17 %). Hitches below 25 remain in both modes when the view turns
(scene streaming/shader work), so a hard 30 floor while moving is not claimed.
The end-of-motion screenshot (Link on the bed, Navi, lanterns) renders correctly.

Note: two early motion attempts died with SIGPIPE because the board's /var/home filled
up (the relay's `tee session.txt` hit ENOSPC); not related to the change. Keep >1 GB free.

## Things that did not help on this hardware

- `-march=znver2 -mtune=znver2` (no RDSEED on BC-250, so `-mno-rdseed`): 27.9 vs 28.1, no gain.
- Reactive flushing off: 25.3 (worse).
- Fastmem off (no mprotect at all): 19.1 — the page-table path costs far more on the guest
  cores. Fastmem must stay; only the protection churn had to go.
- CPU idle/frequency tuning: not available — the board has no cpufreq or cpuidle driver.

## Remaining hot spots (for later work)

- Guest cores spend ~25 % of time in `sched_yield` idle loops (~200k syscalls/s per core) and
  kernel SRSO return thunks; harmless for FPS while GPUBack is the bottleneck.
- Front GPU thread: `GpuSplit::Drain` spin waiting on GPUBack.
- GPUBack after the fix (s6-m6 profile): `UpdatePagesCachedCountNoLock` 13.6 % → 2.3 % of
  its samples, its kernel share 2.8 % → 0.8 % of all samples. It is still the busiest thread;
  draw preparation (`RasterizerVulkan::PrepareDraw`, ~48 % of GPUBack: pipeline configure,
  stage-buffer binds, uniform uploads) is the next target.
