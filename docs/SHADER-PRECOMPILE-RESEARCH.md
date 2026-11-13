# Pre-compiling shaders at launch (2026-11-13)

Worktree `~/Code/EglantinePS5-wt-zmem`, branch `fix/attributeless-precache` (from main v1.000.002). Zelda:
Ocarina of Time .nup, PS5 Pro, 1x, porch, 240 s runs. Figures: the loader's `Shader cache ready: N pipelines in
T ms`, the renderer's `Runtime pipelines in the last 10 s: N built, T ms on the GPU thread` (summed per run:
pipelines built mid-game, each a stall of the GPU thread), the free pool in the load window. Kit:
`.local/zmem/pcsum.sh RUN...`, `pc.sh`, `pc2.sh`.

## How it works today

- The launch already builds the cached pipelines before the game starts (`headless/main.cpp` →
  `LoadDiskResources`, blocking, on the shader workers): the guest shaders are translated again and the driver
  binaries come from `lichii-cache.v2.driver.bin` (VK_KHR_pipeline_binary, 955 pipelines, 43 MB).
- Only the pipelines that recent sessions used are built at launch (the cache's "used" lists, 16 sessions). The
  rest (196–210 for Zelda) are built by a background prebuild during play (`precache_shaders` Off, the default)
  or on the loading screen (`precache_shaders` Always, not exposed on PS5).
- A new app build does not invalidate the driver binaries: launches stay at 3.6–6.3 s across builds.

## The bug: ~80 pipelines rebuilt mid-game every session

Every warm session still built 78–81 pipelines on the GPU thread (1.7–2.2 s of stalls), the same ones each time
(58 of 62 alike in two runs in a row). They are in the cache file and on its used lists, but the launch skipped
them: they belong to vertex shaders that read no attribute, which the PS5 port draws with a static key (no
dynamic vertex input, `CurrentGraphicsPipeline`); `KeyFitsDevice` rejected such a key because the device has
dynamic vertex input. The old patch set accepted it (`EdenAttributelessStaticKey`); that hunk was lost when the
patches were folded into `Eglantine/` (983612f), leaving the helper unused. Each session rebuilt them and wrote
them again (the "superseded" records).

Fix (`vk_pipeline_cache.cpp`, `KeyFitsDevice`, PS5): accept the static attributeless key when the device has
dynamic vertex input and static vertex input is on.

| Run | Build | Launch | Built mid-game | Load-window min free | FPS |
|---|---|---|---|---:|---:|
| hpfin1 / hpfin2 | before | 661 in 4.4 / 4.7 s | 78 / 81 (2.2 s stalls) | 938 / 786 | 30.1 |
| pc1a / pc1b / pcdef | fix | 745 in 5.1 / 6.3 / 5.8 s | **0** | 1018 / 794 / 780 | 29.3–30.0 |

Same picture (porch screenshots).

## Precache modes (with the fix; `EDEN_PRECACHE` was a test switch, not in this branch)

| Mode | Runs | Launch | Mid-game | Min free | Heap peak |
|---|---|---|---|---:|---:|
| Default (210 built in the background during play) | 3 | 5.1–6.3 s | 0 | 780–1018 | 1544–1608 |
| `EDEN_PRECACHE=always` (built on the loading screen: +1.2 s) | 1 | 6.3 s | 0 | 860 | 1512 |
| `EDEN_PRECACHE=used` (never built) | 2 | 4.9–5.2 s | 0 | 743–998 | 1512–1544 |

All within run-to-run noise: for a warm cache the mode does not matter; the fix does.

## When the launch cannot help

- **First launch / no cache** (cold1, cold2, hpcold1): nothing to build at launch; 768–778 pipelines are built
  during the first load (12–24 s of GPU-thread stalls) with the heap at 2.3–2.8 GB and the pool down to
  12–114 MiB. Only a cache from elsewhere would help: the guest cache (`lichii-cache.v2.bin`) holds guest
  shaders and pipeline keys, not driver binaries, so it should carry between consoles of the same app version
  (not tested); an import (drop a game's cache in a folder, the launch builds it)
  would turn the first session into a ~5 s launch plus driver compiles on the loading screen, when the pool is
  free.
- **After a setting that changes shaders** (resolution 0.5x/0.75x): the driver binaries no longer match, and
  the launch compiles ~800 pipelines in the driver: 17–19 s once, cached for later launches.

## Recommendations

1. Merge the `KeyFitsDevice` fix (removes ~2 s of mid-game stalls per Zelda session; any game with
   attributeless vertex shaders benefits).
2. Keep the precache default; consider exposing "Precache shaders: Always" for players who prefer a 1 s longer
   launch to background compiles.
3. For the first session: a cache import (or a per-game cache download) is the only way to pre-compile; worth it
   given the cold load is also the memory-risk moment.
