# BC250: Zelda frame-generation pacing (2026-11-15)

Complaint: stutter with frame generation on, in **desktop mode** (KDE Plasma/KWin, Xwayland
window, 4K 60 Hz, no VRR). User settings: Optical Flow (`frame_gen_type=1`), multiplier 2,
**`frame_gen_target_rate=60`** (per-game), FIFO (frame generation forces it).

## How frames reach the screen

`RendererVulkan::Composite` presents the generated frames back-to-back immediately before the
real one (`renderer_vulkan.cpp`). `FrameGenPacer::Plan` (`present/frame_gen_pacer.cpp`) only
decides how many generated frames each real frame gets. With a target rate it used a running
output credit: `outputs = floor(credit += interval × target)`, so the count varies from real
frame to real frame. Under Xwayland, `vkQueuePresentKHR` returns at once (the generated and the
real frame reach Xwayland 0.1–0.3 ms apart); FIFO spacing happens in Xwayland/KWin.

## Measurement

Desktop-mode harness (`.local/bc250-fgpace/fgdesk.py`): launches the app windowed like the user,
loads the save through the debug server's `input_sequence` (no synthetic desktop input), waits
until uptime 250 s, records 60 s of guest frame times and a 30 s tracefs uprobe trace of
`FrameGenPacer::Plan` (return value = generations), `PresentManager::Present` and
`CopyToSwapchainImpl` (`fgtrace.sh`, symbols resolved with `nm`). Analysis: `analyze.py`.
"Step CV" = coefficient of variation of the motion each output frame advances
(real interval / (generations + 1)); 0 is perfectly even.

## Results (desktop mode, Kokiri Forest save, 1280x720 window)

| run | settings | guest fps | gens per real frame | zero-gen | step CV | presents/s |
|---|---|---|---|---|---|---|
| t60b (static) | user: target 60, old pacer | 34.1 | 1,1,1,0 repeating | 23.5 % | **0.305** | 60.0 (bursts) |
| t60m (walking) | user: target 60, old pacer | 32.7 | 0/1/2 mixed | 19.8 % | **0.290** | 59.7 |
| x2a (static) | fixed 2x (target 0) | 30.0 | always 1 | 0 % | 0.103 | 60.0 (one per vblank) |
| x2m (walking) | fixed 2x | 29.5 | 1 | 1.3 % | 0.130 | 59.3 |
| offa (static) | frame generation off | 34.1 | – | – | 0.086 | 33.9 |
| fix3a (static) | target 60, new pacer | 30.0 | always 1 | 0 % | **0.099** | 60.0 (one per vblank) |
| fix3m1/m2/m3 (walking) | target 60, new pacer | 29.6 / 25.0 / 29.6 | 1 (2x held) | 1.8 / 3.1 / 0 % | 0.140 / 0.239 / 0.104 | 59.2 / 50.4 / 59.7 |

**Cause:** with a target rate the old pacer dithers the generated-frame count. At 34 fps toward
60 it wants 1.76 outputs per real frame, so every fourth real frame gets none: motion advances
14.4 ms per output frame for six frames, then 28.8 ms once — a visible ~8 Hz judder. The present
calls also come in bursts (gen + real 0.1–0.3 ms apart) with uneven gaps.

**Fixed 2x does not have it:** one generated frame per real frame; FIFO back-pressure then
locks the guest to 30 fps and presents one image per vblank (present gaps 12–20 ms).

**Fix (`fix/fg-target-rate-pacing`):** in target-rate mode hold one output multiplier
(`PickOutputs`: 2x from 1.3 wanted outputs, 3x+ only from k+0.85), step up only after 1 s of
clear shortfall, down at once, and past 2x try one step lower every 3 s. `EDEN_FG_DITHER=1`
restores the old credit. Two rejected drafts, kept for the record: rounding with a ±25 % band
(fix1) and the band plus 5 s down-trials (fix2) both locked walking into 3x, which FIFO turns
into a 20 fps guest (fix1 walking 20.2 fps, fix2 22.1 fps).

Trade-off: a 34 fps scene now runs at 30 fps (FIFO-paced 2x), the same as fixed 2x; the step
CV left (0.10–0.14) is the guest's own frame-time jitter. Scenes below ~24 fps stay at 2x
(50 presents/s, fix3m2) instead of jumping to 3x and 20 fps.

**Setting workaround without the fix:** `frame_gen_target_rate=0` (fixed 2x) behaves the same.

## Notes

- Sessions in desktop mode stop composing when the monitor sleeps (DPMS): screenshots then show
  the last frame. The harness keeps the screen on with `kde-inhibit`.
- Gamescope (game mode) was not re-measured here; the earlier 2x run there showed 55 host
  presents/s at 28 guest fps.
