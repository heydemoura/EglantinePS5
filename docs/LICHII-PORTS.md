# Lichii ports (November 2026)

Changes taken from Lichii (zortos293/Lichii `nx2`) into the EglantinePS5 core as patches. Lichii is
about 1,100 commits past our core pin (Eglantine 3140648a), so these are ports, not cherry-picks.
Console numbers: PS5, same save and scene, warm caches, interleaved runs
(`tools/bananza-bench-console.py`, `tools/mkw-bench-console.py`).

## Upload skips (core 0061, 0062)

- **0061, join copy covers the image** (Lichii #817). When `JoinImages` makes a new image that one of
  its alias copies overwrites whole (same format, every level and layer, from the origin, at 1x, on a
  fully backed image), the guest upload and the older copies before it are skipped.
- **0062, stale guest memory** (Lichii #818). When the collector drops a GPU-written image without
  writing it back, its whole pages are marked stale and kept tracked. A new image made entirely on
  stale pages starts without an upload, unless the buffer cache wrote any of those pages on the GPU.
  CPU writes, unmaps and write-backs clear the marks. Switch 2 titles only.
  - PS5 difference: core 0050 also marks texture-cache writes in `g_nx2_gpu_written_pages` (for the
    console's unwritten-page reads), so Lichii's check would almost never skip here. 0062 adds a
    buffer-cache-only bitmap, `g_nx2_buffer_written_pages`, and `Nx2MayBeGpuWritten` reads that.
  - `StalePages::PAGE_SIZE` is `PAGE_BYTES` here: the PS5 SDK defines `PAGE_SIZE`.

Measured: Mario Kart World 1x title, 3 runs each: 23.2 / 27.6 / 26.9 FPS before, 25.6 / 28.2 /
28.4 after (about +5%). Out-of-memory survival is unchanged (both builds run out at about
200-215 s in most runs). Donkey Kong Bananza's Ingot Isle gate: unchanged (16.0 FPS).

## Fast draws and the texture memo (core 0063, 0064)

- **0063, fast draw delta path and texture memo** (Lichii #858 with the #921 and #910 fixes).
  - A draw with the same pipeline as the previous one, after only light state changes (draw
    parameters, index and vertex stream registers, constant buffer binds, and inline constant data
    that reaches neither the texture handles nor the texture/sampler headers, compared by GPU and
    device address), keeps the previous resolve's pipeline, image views, samplers and texture
    buffers, and resolves and binds every buffer again. Anything else forces a full resolve.
  - Breaks run on the split's back thread in order. Every semaphore acquire sends an empty deferred
    call to the back, because several of our acquire paths don't drain it.
  - `vertex_id_base` counts as a draw parameter: nothing in the renderer reads it, and NVN's draw
    macros set it before nearly every indexed draw (about 25,000 chain breaks a second at the gate).
  - The texture memo resolves a pipeline's sampled textures from what its last draw resolved, while
    the handle words, TIC/TSC entries, tables and mappings are unchanged.
  - Switches: `EDEN_NX2_FAST_DRAW=0`, `EDEN_NX2_TEXTURE_MEMO=0` turn them off.
    `EDEN_NX2_FAST_DRAW_VERIFY=1` and `EDEN_NX2_TEXTURE_MEMO_VERIFY=1` resolve in full as well and
    count mismatches; stats log every 10 s with a verify variable or `EDEN_NX2_GPU_PROFILE=1`.
- **0064, buffer sync early-outs** (other #858 parts): `WordManager::ForEachModifiedRange` skips clean
  words, fence operation lists are vectors, and index buffer upload scans are skipped when nothing
  marked the range since the last scan (as core 0018 does for vertex buffers).
- Not ported because the core already has them in another form: alias-sync skip (core 0018),
  empty direct draw skip (core 0019), word-at-a-time GPU-written page marks.
- `tools/check-gpu-sync-stop.py` stubs the two fast-draw breaks its extracted methods now call.

Measured at Donkey Kong Bananza's Ingot Isle gate (2 runs each, two rounds): 15.8-16.0 FPS before,
16.9-17.1 after (about +7%). Verify runs found 0 mismatches (about 1.1 million delta draws checked
every 10 s; the memo serves about 97% of draws). About 90% of draws take the delta path, and
per-draw setup falls from 5.7 to 2.7 microseconds. The GPU back thread is now spent mostly binding
stage buffers (115 ms/s), updating buffers (108 ms/s), pushing descriptors (31 ms/s) and decoding
methods (144 ms/s); Lichii #949 (uniform buffer keeps, descriptor replay) targets the first three.
Lichii measured +27% on a PC.

## Uniform buffer keeps and descriptor replay (core 0065-0067; draft)

- **0065, uniform buffer keeps** (Lichii #949). On a fast draw delta draw, a uniform buffer slot
  the previous draw streamed is bound again from that draw's allocation when the slot wasn't rebound,
  no write overlaps its range (inline constant data by device range, CPU writes the caches were told
  of, GPU-modified marks, DMA clears and copies) and no buffer was deleted. The Vulkan runtime records
  each streamed graphics uniform bind's allocation per stage and slot; a keep needs the same address,
  size and command buffer. `EDEN_NX2_UBO_KEEP=0` turns it off; `EDEN_NX2_UBO_KEEP_VERIFY=1` binds as
  before and counts mismatches.
- **0066, image descriptor replay** (Lichii #949). Delta draws copy the image descriptor entries and
  rescaling words the capturing resolve pushed, instead of pushing them again. The kept entries are
  dropped on every full resolve and unused once the scale epoch changes (ScaleUp, ScaleDown,
  InvalidateScale). `EDEN_NX2_DELTA_DESCRIPTORS=0` / `EDEN_NX2_DELTA_DESCRIPTORS_VERIFY=1`.
- Not ported: #949's drain-free semaphore acquires. Our acquire code already skips the drain for met
  acquires and acquires met by a queued release, and polls with a 2 ms drain valve; the rest needs
  split-thread pieces this core lacks (#639's back idle hook, Lichii's queued-write record).

- **0067, fast draw switch read at first use.** The app sets its environment from `env.txt` in
  `main`, after static initializers ran, so a switch read in a namespace-scope initializer never sees
  it. `EDEN_NX2_FAST_DRAW` (0063) was read that way and is now read at the first use; 0066's two
  switches are function-local for the same reason. New switches must follow this rule.

Bananza Ingot Isle gate, FPS per run:

| Build | Variant | Runs |
|---|---|---|
| main (7274ca2) | default | 14.45, 16.96 (16.9-17.1 in the fast draw rounds) |
| 0065+0066 | default | 17.60, **broken**, 17.74, 17.75, 17.76, 17.74 |
| 0065+0066 | `EDEN_NX2_UBO_KEEP=0` | 16.81, 16.96 |
| 0065+0066 | `EDEN_NX2_DELTA_DESCRIPTORS=0` | 17.71, 17.38 |

- Uniform buffer keeps are worth about +5% (16.9 to 17.75). Descriptor replay is within run noise
  (0 to 2%).
- Uniform keep verify (2 runs): 3.5-3.8 million binds checked per 10 s at the gate, 0 mismatches.
- **One broken run, not reproduced.** In the second run of the first round, draws with 4294967295
  indices arrived from 165.2 s, then 491 "Native MME ... instruction limit exceeded" errors, and
  from 173 s the frame rate fell to 0.1-3.6 FPS with missing geometry. None of the 10 later runs with
  the features on, nor 14 earlier runs of other builds, show it. The cause is unknown, and the
  features are neither confirmed nor cleared as the cause.
- **Not yet run:** the descriptor replay verify run and an `EDEN_NX2_FAST_DRAW=0` run with the fixed
  switches (the console went down before them). The first verify round did not check descriptor
  replay, because its switch was not read.
- `EDEN_NX2_FAST_DRAW_VERIFY=1` sends every draw down the full path, so it disables both features;
  verify them with their own variables only.
