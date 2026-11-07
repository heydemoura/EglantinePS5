# Donkey Kong Bananza: Ingot Isle neon gate frame rate

At Ingot Isle the frame rate fell from 60 FPS to about 9 FPS whenever the neon gate and its
scaffolding were in view (camera turned away: about 55 FPS). Half resolution only reached 12 FPS,
so the cost was not pixel shading.

## What the frame does there

Measured on the console with a draw histogram, the split GPU thread's zone profile, ps5-perf
(on-CPU and off-CPU) and counters for render passes, image copies and guest barriers:

- The game issues about 12,000 draws a frame facing the gate (6,700 into the G-buffer and 5,200
  into two 2048x2048 shadow maps), against about 1,700 facing away. Almost all are single-instance
  draws of small meshes.
- One G-buffer target (format 40) is bound as a 2112x1179 colour target while the same draws
  sample the same memory as a 2095x1179 texture. The texture cache keeps two aliased images for it
  and, before each draw that samples the texture after another draw wrote the target, copied the
  whole target: about 1,600 full-screen copies and 1,900 render-pass breaks a frame, roughly
  40 billion pixels a second. The guest issues only about 60 texture barriers a frame there.
- With those copies the GPU was the limit (the Vulkan worker blocked in submission, and the GPU
  thread's back half waited at every present); CPU-side savings alone barely moved the frame rate.

## Changes (tools/eglantine-patches/0018-texture-cache-feedback-copies.patch)

1. **Feedback copies.** A draw that samples an image whose newer alias is one of its own colour
   targets copies that target at most once per guest texture barrier (`fragment_barrier`,
   `invalidate_texture_data_cache`, `tiled_cache_barrier`) and render-target set. Without a barrier
   the hardware does not order a draw's texture reads after earlier colour writes either.
   `EDEN_NX2_FEEDBACK_COPIES=exact` restores a copy before every draw.
2. **Alias walks.** `SynchronizeAliases` walks an image's aliases only when one was modified,
   synchronized or linked since the last walk (a 64x64 texture there has about 590 aliases and was
   walked 24,000 times a second), and `MarkModification` walks overlaps only when the overlap graph
   changed (the main depth buffer has about 140 overlaps).
3. **Vertex buffer synchronization.** A vertex buffer slot whose range had no CPU-modified mark
   since its last synchronization skips the page scan (91% of 580,000 scans a second there).

## Results

Same console, same save and scene, warm caches, 1x resolution, interleaved runs
(`tools/bananza-bench-console.py`):

| Build | Facing the gate (two runs each) |
|---|---|
| Before | 9.3-9.4 FPS |
| Diagnostic build with all changes, `EDEN_NX2_FEEDBACK_COPIES=exact` | 10.1 FPS |
| All changes | 14.8-15.0 FPS |

Facing the gate, image copies fell from about 17,600 to 2,600 a second (pixels copied: 40 to
1.8 billion a second) and render-pass starts from 19,000 to 4,900 a second. The GPU is no longer
the limit there: the GPU thread's back half (about 10 microseconds of CPU per draw) is. Camera-away
rates are not compared: the scripted turn is a timed stick hold, and the game turns further at a
higher frame rate, so the views differ between builds.

## Reproduce

```
python3 tools/bananza-bench-console.py --launcher LAUNCH-BANANZA.elf \
  --candidate base=BASE.bin,BASE.elf --candidate new=NEW.bin,NEW.elf --runs 2 \
  --events '55:a,70:a,85:a,100:a,165:shot,180:cam3000,215:shot' --seconds 225 --out .local/neon/RUN
```

The reference save loads at the gate after the A presses; `cam3000` turns the camera away.

AI assistance: the investigation, the changes and this document were made with Claude Code.

## Other titles

Mario Kart World's title (1x, interleaved): 22.3 FPS median before, 23.6 after, inside its
run-to-run spread; its feedback copies are about 25 a second either way, so the coalescing rarely
applies there. Both builds still run out of device memory at 1x within about two minutes (known).

## What limits it now, and what did not help

With the copies gone, the GPU thread's back half is the limit: about 12,000 draws a frame at a few
microseconds of CPU each (uniform-buffer uploads, vertex/index binding, descriptor and texture
lookups, method dispatch), and it waits ~18% of the time for the front half, which drains it at
semaphore acquires and before reading GPU-written command lists. Tried on top of the patch,
same binary with and without each change, two runs each at the gate:

| Experiment | Gate FPS |
|---|---|
| Download a GPU-written list's whole 1 MiB block in one wait (GPU waits 740 to 120 a second) | 15.2 vs 14.9 |
| Let an acquire that a queued release will meet proceed without draining the back | 15.4 vs 15.2 |
| `EDEN_NX2_UBOREUSE=1` (existing switch) | 8.9 vs 9.8 (before the patch) |
| `EDEN_NX2_ACQUIRE_DRAIN_TO_RELEASE=1` / `EDEN_NX2_LIST_DRAIN=recent` (existing switches) | no change |

Neither of the first two is in the patch: the gains are within 2% for changes with wider reach.
Going further needs cheaper draws on the back half (or splitting draw processing across threads).

## Reusing a draw's whole setup (tried, not kept)

A draw whose pipeline, texture handles, samplers, TIC entries and view images (nothing to upload
or copy) and uniform buffers (same bindings, byte-identical streamed bytes, no CPU write to bound
buffers) matched the previous draw of that pipeline in the same command buffer pushed that draw's
descriptors again instead of preparing them. At the gate, per 5 s: ~340,000 draws qualified,
~171,000 failed the texture check (an image to refresh or an alias to synchronize), and 99% of the
rest failed the uniform-buffer check: the game uploads per-object constants for nearly every draw.
0.5% of draws reused their setup, which did not pay for the checks (15.25 vs 15.33 FPS).
