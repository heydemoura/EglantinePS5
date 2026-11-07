# JIT fast paths (Phase 3c)

Backported from ProsperoEden (GPL-3.0-or-later). The code is `headless/dynarmic/fast_paths.cmake`,
wired in `headless/CMakeLists.txt`. It adds four x64 code-generation changes for patterns that
were hot in a racing game's profile:

| Switch (env.txt) | Change |
|---|---|
| `EDEN_JIT_FCMP_ADC` | FCMP/FCMPE flags: `adc cl, cl; shl cl, 4` instead of `rcl cl, 5`, which Zen 2 runs as microcode |
| `EDEN_JIT_LIGHT_DMB` | DMB `*LD`/`*ST` emit no host fence (x86 ordering already holds). Full DMB uses `lock or [rsp], 0` instead of `mfence` |
| `EDEN_JIT_CHECKBIT_FLAGS` | CBZ/CBNZ/TBZ/TBNZ terminals branch on the setter's flags instead of a stack store and reload, when the bytes in between are verified |
| `EDEN_JIT_FRAME_ONCE` | A64 blocks' frame pointer set once per RunCode/StepCode entry instead of saved, set and restored in every block |

**On by default** (since Phase 3d). `EDEN_JIT_FAST_PATHS=0` turns all four off, and `=0` on a single switch turns that one off. The switches are read once, when the first JIT is
created (after `env.txt` is applied), so the run-code prelude and every block agree. The log
prints `EDEN_JIT_FAST_PATHS fcmp_adc=… light_dmb=… checkbit_flags=… frame_once=…`.

## Validation

- `eden-memory-check` runs every JIT check with all four on, and adds `CheckFastPaths`: 52 cases
  covering FCMP flags for single and double (including NaN and signed zero), DMB ISHLD/ISHST/ISH,
  and CBZ/CBNZ/TBZ/TBNZ with each path on and off. All checks pass, including the direct table
  and page reuse.
- Console, Mario Kart World 1.8.0 at **1x**, warm caches, interleaved, 180 s each:

| Pair | Direct table (FPS) | + fast paths (FPS) |
|---|---|---|
| 1 | 16.57 | 16.26 |
| 2 | 24.64 | 30.03 |
| 3 | 22.63 | 29.78 |
| Median | 22.63 | 29.78 |

A separate cold-cache run with fast paths reached 24.4 FPS with no crash in 150 s. Screenshots
confirm the title renders correctly. The first pair ran while the session's caches were still
warming, which kept both sides low.

Every crash in these runs happened with 1–3 MiB of the pool free: the known exhaustion
(docs/MKW-MEMORY-BUDGET.md). "Thread constructor failed" is the same exhaustion, and it appeared
in a direct-table-only run.

The paths stay opt-in pending a longer qualification and other titles. Title screens do not
exercise every DMB and branch pattern.
