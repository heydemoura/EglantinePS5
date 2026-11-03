Mario Kart World driver validation
=================================

Worktree: `EglantinePS5-mkw-driver`, branch `perf/mkw-ps5-vulkan-driver`,
based on `dbf0275`. The original checkout and the concurrently active
`EglantinePS5-mkw-review` worktree are preserved. The baseline uses the
existing four maintained patches on core `3140648a668aaebf87bae2dea17512fdca125ba0`.

Native Vulkan builds now default to RADV from mihawk-99/PS5_Vulkan. The legacy
CUSTOM selection remains explicit. Isolation, configure, native linking and
packaging validate the release archive against its recorded digest, pinned
Mesa and SDK revisions, and assertions-off provenance. Preparation regenerates
an isolated archive when its receipt no longer matches. A package frontend
receipt records the selected source/isolated archive hashes and revisions.
This prevents a locally staged diagnostic archive being silently accepted as
the release driver simply because the file exists.

Selected driver: Mesa `7b59ef27c1b09b9671bc4153c41940c3155c3af2`, SDK
`b83202be73e930050e00de0f1b4d9ec46c0391bf`, source archive SHA256
`f07901a34187d63dc64b1abd43218dc6b0d30a7f7f0ad0c8c5224e85a984d0d9`.
It includes this repository's existing display/platform adaptations; it is
not claimed to be a pristine upstream archive. The initial namespace-only isolation SHA256 was
`f4cdf45b40c0dff3512c485887f4f028ca0f2a76e0ef3c22fa915aa06d26c8bf`.
The retained clock-adapted artifact is documented at the end of this report.

Archive inspection compared all 703 object members with the earlier diagnostic
archive `78472eb487446418b222d33f5b7895eb3e1386dac0e228f3612ba4bc0166d4b6`.
Only `winsys_ps5_radv_ps5_platform.c.o` differs. The archived source patch adds
sparse-allocation logging and retains the original 2 MiB zero-block mapping
behavior. Removing that instrumentation alone is not evidence of an FPS gain.

Validation
----------

`tools/check-radv-artifact.py` exercises the real isolation CLI with a small
archive: public entrypoint retention, private implementation symbol renaming,
input preservation, and rejection of missing/stale/modified archives and
mismatched source path, release receipt, Mesa pin, SDK pin or assertions mode.
It passed. Shell syntax and Python compilation checks passed. The maintained
patch reproduction check passed: five patches, 130 files, matching the core.

A source synchronization failure exposed hashing of untracked submodule paths
from the outer checkout. The synchronization script now hashes them from the
submodule directory. An isolated Git fixture verified copying an untracked
nested filename containing spaces from an unrelated working directory, and
that editing its contents changes the synchronization stamp and copied data.

The >40 FPS title-screen objective remains open; console results follow below. Diagnostic candidate builds retain the inherited intrusive
instrumentation and must not be described as production performance tests.
The existing broader investigation is in `MKW-PERFORMANCE-REVIEW.txt`.
Private console logs, profiles, ELF carriers, game data and binary archives
are kept outside tracked source.

Guest acquisition context
-------------------------

Offline analysis decoded the captured SDK's GNU-hash dynamic symbols and its
NVN procedure-name/offset tables. In historical run
`upload41-run-1793650997402206680`, the loader placed the SDK at `0x85024000`;
the dominant read context `0x856894d0` therefore resolves to SDK+`0x6654d0`.
The carrier's SDK NSO SHA256 is
`acdc4a29bb809955d6a680f3f8515e0bd1d6eb1028e6a38182f00dea08c309bd`.

The named `WindowInitialize` entry at SDK+`0x673260` obtains a backend object
from SDK+`0x6767e0` and stores it at SDK+`0xfe4078`. That factory installs the
vtable at SDK+`0xe16038`. `WindowAcquireTexture` (SDK+`0x673800`) loads the
same object and calls its vtable+`0x10`, which contains SDK+`0x665430`.
The dominant context is inside that routine. Following its virtual callback,
it calls SDK+`0x66fa40` with timeout -1; the named `SyncWait` entry at
SDK+`0x66fa30` branches to that implementation.

This is a static reconstruction using the matching NSO image, derived ELF
carrier and historical loader base, not a new runtime instruction trace.
Runtime replacement of indirect callbacks remains possible. It identifies
frame acquisition/synchronization as the context for the dominant read and
helps target preceding-work attribution. It does not establish that the wait
or guest-visible completion ordering can safely be bypassed. REA reported
no configured native decompiler, so LLVM disassembly was used directly.

Release-driver baseline build
-----------------------------

The native diagnostic candidate built and packaged successfully. Executable
SHA256: `cb6a6fb51bb20be1e9de70b7567f14a93f24f6ccc396008bfaabdd11e970efcb`;
matched ELF SHA256:
`8ed7359fdb3617916ac3ef46415259c4c74a3f6cb742ccaa0ca22702687290a4`.
RADV/OpenGL native link checks passed, including entrypoint identity, heap
ownership, C++/unwind ABI and IEEE startup. Generated allocator-header checks,
scheduler handoff, temporary-image lifetime, timestamp-ring and fence-mask
fixtures passed. This diagnostic build used `EDEN_SKIP_SOURCE_CHECKS=1`;
it does not establish that the broader inherited release source checks pass.
The complete inherited draft still needs its release instrumentation review.

Console results before gating dump files
---------------------------------------

Diagnostic PID 649, run `1793663022160875013`, restarted at 104 s with
`VK_ERROR_OUT_OF_DEVICE_MEMORY` in the Vulkan submission worker. The exact
matched crash report records 1 MiB largest free memory, 2240 MiB heap backing
and 396 MiB in large blocks. This run did not reach the title-screen gate;
its early 53 FPS loading window is not title performance evidence. The crash
report and archived logs were collected and symbolized against the matched ELF.

Diagnostics-off PID 652, run `1793663145528627892`, reached a viewed Mario
Kart World title screen. Both initial and final captures showed road, terrain,
Mario, vegetation and foreground objects beneath the title logo. It used the
existing effective per-title 0.5x resolution, Bilinear, 1080p/60 Hz output;
settings were observed rather than changed. Eleven conservative full windows
between the title and settled captures covered 55.486 s and 594 frames:
10.705 FPS, worst frame 1512.950 ms. These exclude the profiler and edge
intervals; remaining streaming/compiler stalls are included. This does not
meet the performance objective.

The diagnostics-off executable SHA256 was
`b8247fab1c88f726b7b1f5e08353429dd744660853afc0b743e158a4fcc68f7a`,
matched ELF `13adf81af1f0034cd7b6584f9f5a47ddde7613aad8a794c84b890ff198fec3ff`.
ps5debug verified PID, title and mappings. A bounded elfldr helper launched
the game through its ordinary `--rom` argument, and the existing klogsrv
captured runtime output. ps5-perf collected 584 samples over 3 s at 99 Hz
with the matched ELF: average stop 136.1 us, maximum 655.4 us. Many leaf
samples in `sub_380` resolve through parent stacks to pthread condition-variable
operations; they must not be mislabeled as an unknown shader hotspot.

Seven GPU-thread samples (of 584 process samples) passed through
`TranscodeAstcToBcn`, file close/flush and `_write`. The inherited core emitted
per-transcode file records and allocation snapshots even with
`EDEN_MKW_DIAGNOSTICS=OFF`. New maintained patch 0005 gates those dumps and
allocation-statistics scans behind the existing explicit
`EDEN_NX2_READBACK_DIAGNOSTICS` setting. It retains allocation, synchronization,
transcode output and ordinary error reporting. Five patches reproduce all 130
modified/new core files. A new native candidate with this patch built and
packaged successfully; its console measurement follows below.

All experiments use the shared console lease. The first two completed runners
restored and hash-verified their original executable, metadata, settings and
input controls. No FPS gain or broad stability is established by these runs.

Retained dump-gating candidate
------------------------------

Candidate `a2de251b016734e728801006cd9fafa24fba193f78019dc94043ab5e260797bb`
(matched ELF
`5441cb31cffe9dd1b15c13d688ef8813ba0f9334022a4b3ab7d58e49aaaba97c`)
ran as fresh PID 655 in run `1793663553166611474`. Initial and final viewed
title captures showed Mario, road, palace/desert scenery, palms and background
objects. Conservative full windows between the viewed title and settled
capture covered 85.757888 s and 1132 frames: **13.199952 FPS**, worst frame
492.039 ms. The runtime launch log records 1x resolution. This corrects the earlier
0.5x attribution, which belonged to the preceding run.
The attract scene and effective resolution differed from PID 652, so the two
FPS results do not prove a controlled speedup. Neither result meets >40 FPS.

The three per-process transcode/image/buffer dump files were all absent.
ps5-perf recorded 902 samples over 3 s, with mean stop 131.5 us and maximum
580.9 us. Transcode execution was present in sampled stacks (30303 us summed
sample weight); its file-I/O stack weight was zero. Sampling is bounded and
is not proof about every possible transcode path. The source gate plus absent
fresh-process dump files establish that those diagnostics stayed disabled in
this tested configuration. No rendering failure or fresh crash occurred in
this bounded title observation. The original executable, metadata, settings
and input controls were restored and hash verified before releasing the lease.

Native build/package and link ownership/ABI checks passed after the patch.
At this stage, the broader `tools/check-performance.py` still failed its first
JIT `Eden::Performance::Timer` exclusion. The subsequent CPU instrumentation
cleanup below resolves that source check. Memory/cache/completion costs and
the >40 FPS objective remain open.

Useful next evidence: separate repeated texture eviction/recreation and ASTC
opacity/transcode work from frontend completion waits. The uninstrumented-by-
MKW-build-flag profile still contains ordinary condition-variable work and
CPU-side texture processing. Earlier negative notification experiments in the
performance review should not be repeated as presumed fixes. Memory usage
also differs substantially across runs, so an OOM should not be attributed
to the logging-only driver-object change without controlled allocation data.


Production CPU instrumentation cleanup
-------------------------------------

The retained build now limits guest-run timing, guest stack probes, wait-state
accounting and idle clocks/counters to explicit MKW diagnostics or development
profiling. Ordinary scheduler, idle-wakeup and thread-lifetime behavior stays
in place. The production source check additionally rejects these probes from
both JIT wrappers, KThread and PhysicalCore. Diagnostic wrapper timers remain
allowed only when MKW diagnostics are explicitly enabled.

With `EDEN_SKIP_SOURCE_CHECKS=0`, the CPU-cleanup build passed the native source
and host checks, including 179 cases / 2,239,932 assertions, JIT aliases and
exclusive operations, GPU completion/queue fixtures, native allocator layouts,
RADV/OpenGL ownership and unwind ABI checks. Five maintained patches reproduce
all 130 modified/new core files. These checks do not establish >40 FPS.

Candidate executable SHA256:
`c40178ebf0f1fdaedccbe3fa22271e495f1949063c388a819e08cb01b2c82a31`.
Matched ELF SHA256:
`e7cebce186963427d9a9c941cad455eb959ea440babf667e8100c7ae0923eb34`.
Run `1793664768687396372`, fresh PID 661, used 1x resolution, Vulkan/Bilinear,
1080p/60 Hz output and handheld mode. Viewed captures show city-road traffic,
Mario, guardrails and background structures. Conservative full title windows
before profiling cover 80.670509 s / 1004 frames: **12.445688 FPS**, worst frame
774.900 ms. The selected attract scene differs from PID 655; these measurements
provide no controlled FPS gain. The final capture had a stale FPS HUD during
a possible stall; this bounded test does not establish broad stability.

The three dump files were absent; there was no fresh boot restart or collected
crash. ps5-perf collected 875 samples over 3 s (mean stop 122.2 us, max 763 us).
Its separate 3.006 s thread-stat interval measured 8918.75 ms process CPU time
(2.967 CPUs), 73,680 context switches and 2,101 involuntary switches. GPU used
1350.11 ms; the six guest workers used 997–1216 ms each. The workers also appear
in host idle spinning and completion waits. Sample counts include sleeping
threads and must not be interpreted as CPU utilization. Recorded CPU fields
were all zero and cannot establish actual worker CPU placement. End-of-run
largest free physical block was 254 MiB; memory pressure remains relevant.

Temporary VMA block-size experiment
----------------------------------

A private 16 MiB preferred-VMA-block candidate, retaining the same CPU cleanup,
passed the full native checks and ran as PID 663 in `1793665003813256821`.
Executable SHA256:
`c7e0b4eb495cf2f31d144a2c0eb94a215a6f02ea53cb9ca112e527a4bf07582c`;
matched ELF:
`c57a05b15ef4ef33e835be687a06a72e3bd53a371278efc0ff7ce322fc0e23b0`.
The runtime marker confirmed the 16 MiB path. Both viewed captures rendered
Mario, snow/ice road, trees and rock walls. At 1x, conservative title windows
covered 20.163428 s / 295 frames: **14.630449 FPS**, worst frame 1007.389 ms.
It selected a different attract scene from PID 661, so neither the FPS nor the
201 MiB end-of-run largest free block establishes an allocator improvement.

The three dump files stayed absent. ps5-perf collected 885 samples over 3 s
(mean stop 124.9 us, max 2668 us); thread stat measured 2.890 process CPUs,
79,659 context switches and 1,739 involuntary switches. No fresh crash or boot
restart occurred during this observation. Both runners restored and verified
the original executable, metadata, settings and input controls before releasing
the console lease. The unproven allocator option was removed; the retained
source uses the existing VMA block policy. These runs do not meet >40 FPS.


Retained rebuild after allocator experiment
------------------------------------------

The restored default allocator build passed the same full native checks and
packaged executable
`cf803591a96aec36a01b7b0ef17795bdc5edf4798aeff16c6e69b3812df362d8`,
matched ELF
`6a396871e3ef8a35b596f157117aee5f85bc5ed4d2e73b7e4d2c5cf69d97e028`.
Compared with the console-tested CPU-cleanup build, text, initialized data,
GOT and lifecycle arrays match byte for byte. Read-only data differs in three
bytes of the generated build timestamp (00:01:19Z versus 00:26:49Z). The
comparison receipt is retained privately with the matched ELF. A fresh final-
artifact console observation and optional idle-spin A/B/A are queued.

The optional test changes only the mapped four-byte native idle-spin atomic,
after matching live text and its initial value against the exact ELF, then
restores and reads back the initial value. It skips mutation if identity fails.
The command handshake follows the upstream
[ps5debug-NG wire protocol](https://github.com/Pharaoh2k/ps5debug-NG/blob/master/PROTOCOL.md).
This is a diagnostic experiment, not a retained runtime setting.


Pending driver wait investigation
---------------------------------

In the pinned driver source, `radv_ps5_sync_wait_many` supplies a
`TIME_MONOTONIC`/`os_time_get_nano` absolute deadline to plain `cnd_timedwait`.
The matched native ELF confirms that its isolated C11 `cnd_init` calls
`pthread_cond_init` with NULL attributes, and `cnd_timedwait` forwards the
deadline directly to `pthread_cond_timedwait`. Mesa's timeline implementation
uses `u_cnd_monotonic_init/timedwait` instead. A default realtime condition
clock would make this particular unsubmitted-signal wait expire immediately.
A bounded payload measured the actual default clock and 5 ms waits on
this console. Run `1793666418607575717`, probe PID 677, reported default
clock 0 (realtime), versus monotonic clock 4. Three default-condition waits
with monotonic deadlines returned ETIMEDOUT in 2.245–5.052 us; three conditions
initialized with CLOCK_MONOTONIC returned after 5.016–5.111 ms. Probe ELF
SHA256: `a7ea1f347512a5578cfe20b788c052c2591216a2da0be1e3c031ee4ca2aaae6b`.
The probe ran as a separate, bounded payload and exited; it does not measure
the frequency of this path in the emulator. Its output file was collected
and removed. The runner then stopped before package deployment because FTP
reported successful deletion with response 226, which Python ftplib raises
as error_reply; the retry accepts that successful reply. Saved title samples did
not catch this condition-wait leaf, so no title-path coverage or performance
benefit is established. The separate submitted-queue wait also busy-polls
completion and host time for up to 1.5 ms; its clock calls appear in the
saved profile. Those two waits must not be conflated.


The user subsequently requested GPT-6.1 Terra for screenshot analysis. That
model re-inspected title/end captures from PID 661 and PID 663. It confirmed
rendered attract scenes with Mario and terrain and no obvious missing geometry;
PID 661's end HUD has no numeric FPS reading. It did not infer FPS or stability.

A private clock-fix derivative compiled successfully from the pinned driver
headers/sync source and the reviewed exported winsys source (the latter adds
three queue-creation error messages to the pin). It changes the queue's
condition type and initialization/wait/broadcast calls to Mesa's monotonic
helper. Native compile-time checks require identical condition size/alignment.
Archive inspection finds all 703 members present, with exactly the sync and
winsys objects changed. Private isolated archive SHA256:
`54e0c134e3ace875fe33f5ef3d57e4ec8b7dde23a3f1f49e9a1e8ae45846afba`.
The original release archive and its namespace-isolated copy are unchanged.
The derivative linked into private ELF
`582708ff40f95575cf69d6c736eed2deb615906dd06259ce4bcad2fab65fff51`
and packaged executable
`b4248eefa22f87a8139890c7c7a45f9e361f0342e568523043d60fd58da0f691`.
Native ownership/unwind ABI and load alignment checks passed. It has not
yet received console qualification or a full driver CTS run. Its source,
compile/link commands, object hashes and exact ELF remain private receipts.


Final CPU-cleanup artifact observation
-------------------------------------

Run `1793667480803908944`, fresh PID 688, deployed and verified the retained
`cf803591...` executable and exact matched `6a396871...` ELF. Its saved JSON
and runtime launch report show 1x, Vulkan/Bilinear, 1080p/60 Hz and handheld.
GPT-6.1 Terra viewed title and settled captures: Mario, paved road and castle
scenery, then a close overhead view including paved/sandy ground and a vehicle.
No obvious major missing geometry/corruption was visible. The camera moved;
these images do not establish identical scenes or sustained stability.

Four conservative full windows before stat/profile activity total 338 frames
in 20.139756 s: **16.782726 FPS**, worst frame 120.164 ms. This is a different
attract scene from PID 661 and does not establish a controlled speedup. A
separate 3.004 s stat measured 2.878 process CPUs and 78,730 context switches.
The subsequent record request failed explicitly: `another sampling session is
active`. No stack-profile evidence or idle-spin comparison was produced in
this run. The runner collected logs, restored and hash-verified its original
package/controls and confirmed idle before releasing the lease. This verifies
the final CPU-cleanup artifact's bounded title rendering, not >40 FPS.

Reproducible sync-clock adapter
-------------------------------

`tools/isolate-radv.py` now applies the confirmed correction while leaving the
input release archive unchanged. In just two PS5 winsys object members it
redirects C11 init/broadcast/timedwait references to Mesa's existing monotonic
helpers, before the established namespace isolation. The pinned Mesa helper
wraps one pthread_cond_t at offset zero, matching its C11 condition storage.
The adapter is restricted to the reviewed Mesa/SDK pair and rejects pin changes
until reviewed. The manifest and package receipt record its identity and script
hash; stale adapters fail validation and preparation regenerates them.

This is ABI-equivalent to the privately compiled condition-type/call-site
source change. It preserves other Mesa condition variables and the original
instruction/data bytes in the affected objects; their call relocations choose
the existing monotonic functions. The fixture links and executes the adapted
queue calls, verifies unrelated C11 calls keep their targets, and rejects a
missing sync member and altered adapter receipts. Native compile-time checks
verify SDK condition size/alignment/offset. A machine-code gate rejects the
old matched executable and accepts the rebuilt executable only when queue
creation and sync waiting call the monotonic helpers.

New isolated archive SHA256:
`267eab50cae22668d50a3204c3eb37ecf471a1ae34be7bbd3ef7b51a96965f0a`.
All 703 members remain present; only the same two winsys members differ from
the private source-compiled derivative. The source release remains
`f07901a3...`. The first broader rebuild passed 179 cases/2,239,932 assertions,
but stopped after an in-place edit of its executing Bash wrapper shifted its
read position. Packaging did not run. The stable, failure-chained retry passed the full native checks, including
179 cases / 2,239,932 assertions, and packaged executable
`a1f9d489aec16fa800d093243e4c173ce05e796576a49d0dc6d1ed8636cf2259`,
matched ELF
`109f73d55cb2225165e18e12c5e5e4d4cff3c6ac4b5b8fda0a0373694879b89c`.
The linked call-target and native condition-storage gates passed. Direct
comparison also verifies all 30 and 13 allocated code/data sections in the
two adapted members remain byte-identical to the source archive. Console
run `1793669394010210672` launched as fresh PID 704; Terra viewed a rendered
autumn-themed title scene. Its completed timing, pool snapshots and interrupted
idle-spin comparison are recorded below; no >40 FPS claim is established.


Clock-adapter console result and failed A/B/A completion
------------------------------------------------------

Run `1793669394010210672` deployed the exact `a1f9d489...` candidate as PID 704.
Its runtime launch log records 1x resolution. Terra inspected the initial,
settled and zero-spin captures: rendered autumn title scenes with Mario,
bridge/road, cliffs, trees and structures, with no obvious major corruption.
Four conservative full baseline windows total 257 frames / 20.208190 s:
**12.717616 FPS**, worst frame 693.062 ms. This differs from PID 688's castle
scene; it does not prove a clock-adapter speedup or regression. The >40 FPS
objective remains unmet.

ps5debug matched 32 live main-function bytes against the exact ELF, verified
the mapped four-byte idle-spin atomic at 0x4d79338 held 5000, then performed
and read back the bounded changes. With spin zero, seven full windows total
425 frames / 35.336686 s: **12.027161 FPS**, worst 917.449 ms. Baseline stat
measured 2.583 process CPUs / 68,877 context switches; zero-spin stat measured
2.249 CPUs / 79,690 switches. Scene/camera and memory pressure evolved. There
is no complete A/B/A or controlled performance conclusion; default 5000 stays.

At about 197 s, CreateBuffer returned VK_ERROR_OUT_OF_DEVICE_MEMORY, staging
recovery released 51 MiB and then 20 MiB, but a further buffer allocation still
failed. The GPU worker reported OOM and the core stopped. The native process
returned to the launcher without changing PID or boot marker. Terra confirmed
the restored-spin start image was black and its end/final captures showed the
launcher's graphics-memory error. That entire phase and its launcher CPU
statistics are excluded. The original spin value was read-back verified at
5000; package files/metadata/controls were restored and hash verified, with
idle confirmed before the lease ended. All three per-PID dump files remained
absent. No sustained stability is established.

The runner's original boot/PID guard missed this in-process core termination.
Private runners now also inspect bounded fresh stderr tails for GPU-worker or
session-failure markers and stop there; identity verification remains separate
so the native atomic can still be restored in the same verified process.

Two valid global direct-pool region snapshots show 552.94 MiB total free with a
466 MiB largest reconstructed gap, then 199.08 MiB free with an 88 MiB gap.
The allocated-region enumeration was bounded, ordered and complete, but was
not an atomic snapshot. The payload's AvailableDirectMemorySize call returned
0x8002000c, so its zero result is not used; gap sizes are reconstructed from
successful DirectMemoryQuery ranges. The third snapshot (10,134 MiB free) was
after core shutdown and is excluded from game-memory comparisons. These data
support substantial physical pressure; they do not show gigabytes of free
memory stranded solely by fragmentation.

The matched heap backtraces identify 201 MiB in 4,864 allocations through
vk_common_CreateShaderModule -> Device::CreateShaderModule -> BuildShader.
This is retained shader-module SPIR-V, not a sampled CPU hotspot. Source review
shows completed graphics pipelines retain shared modules and the shader-reuse
cache also owns them; compute pipelines retain their module. The next candidate
investigates releasing completed pipeline module references and bounding the
reuse cache while preserving asynchronous compiler ownership and shader reuse.
Its implementation and validation are recorded below. Duplicate RAM
sharing is not being re-enabled: its existing source records an earlier FPS
regression and GPU-fence waits.


Shader-module lifetime and bounded reuse cache
---------------------------------------------

Maintained patch 0006 releases graphics and compute pipeline module references
when their compilation task exits, including a caught driver failure. Graphics
configuration selection happens before submitting the task: a worker can finish
before QueueWork returns. The task retains its modules throughout compilation.
Vulkan permits destroying a shader module after pipeline creation while the
pipeline remains in use ([vkDestroyShaderModule reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyShaderModule.html)).

PS5 builds apply a 64 MiB budget to shader reuse, counting emitted SPIR-V copied
by the driver plus each ReusableShaders object's fixed storage. Least recently
used keys are evicted; the existing four-variant limit and environment checks
remain. In-flight compilation references survive eviction. Retired entries are
destroyed after unlocking the cache, avoiding a driver destruction call under
its mutex. Dynamic descriptor/environment storage, allocator overhead, cache
nodes and in-flight modules are outside this accounting: this is not a bound on
total process or GPU memory. Other platforms retain an unlimited budget.

The host fixture compiles the actual cache methods and graphics constructor
scheduling tail against owned mock modules. AddressSanitizer/UndefinedBehaviorSanitizer
and a separate ThreadSanitizer run passed variant checks, weighted eviction,
LRU ordering, shrink/zero budgets, overflow, concurrent access, in-flight
ownership, destructor re-entry, immediate/deferred scheduling and caught build
failure cleanup. The native build gate runs the first fixture configuration.
Native compilation covers the compute path; no mock compute-descriptor fixture
is claimed. All six maintained patches reproduce 132 modified/new core files.

The failure-chained native build, full checks and packaging passed, including
179 cases / 2,239,932 assertions and the clock adapter's ABI/call-target gates.
Candidate executable:
`7050e96937413cf4f5f12bf0e71bf9eb7feb77a4b0fc18b1e068f370c4833e0d`.
Matched ELF:
`444549e7abae72d9a44c8836899ef13851ff5fedd5d83cd83cce2b81074fb179`.
Driver archive remains `267eab50...`; normal production flags and 1x resolution
are retained. Console run `1793672695814567304`, fresh PID 737, confirms the
64 MiB startup policy and actual eviction logs. Terra inspected the initial and two later captures: rendered desert, city and
road title scenes without obvious major corruption. Conservative full windows
before the verified PID change total 1,463 frames / 116.189846 s:
**12.591462 FPS**, worst frame 835.098 ms. The >40 FPS objective remains unmet.
The camera/scene changes prevent a controlled gain claim.

A retained pre-restart heap sample shows the driver shader-module allocation
site at 44 MiB / 1,152 blocks, versus 201 MiB / 4,864 in the earlier run. The
matched ELF resolves its driver CreateShaderModule, Device::CreateShaderModule
and BuildShader addresses. This is evidence of lower sampled retained module
storage, not a controlled whole-process memory comparison. The successful
baseline global-pool enumeration reconstructs 940.27 MiB free and an 872 MiB
largest gap; AvailableDirectMemorySize again failed and is excluded.

The longer observation did not complete. A buffer allocation failed with
VK_ERROR_OUT_OF_DEVICE_MEMORY, staging recovery freed 67 buffers / 1 MiB, and
another allocation failed. The GPU thread terminated and the crash handler
restarted PID 737 as PID 739. The boot guard stopped the experiment; final
heap/stderr/eden logs belong to the restarted launcher and are excluded. Local
pre-restart tails, first-observed frame counters and klog establish the partial
result. No final pool snapshot, stat sample or completed dump-absence check is
claimed. Original package files, metadata and controls were restored and hash
verified, with idle confirmed before releasing the lease. Memory exhaustion
remains unresolved; this patch does not establish sustained stability.


Matched title profile after shader-cache change
----------------------------------------------

Run `1793676188518827211` completed as fresh PID 750 using the same 7050e969
executable and 444549e7 matched ELF. Terra inspected both profile endpoints:
rendered blue coastal-road title scenes, no launcher/error or obvious major
corruption; several distant track-like segments in the first still appeared
disconnected, with intended appearance uncertain. The three dump files were
absent, and original console files/controls were restored and verified idle.

A 3.005322 s stat interval measured 2.993 process CPUs and 94,976 context
switches. GPU used 0.412 CPU / 24,889 switches; VulkanWorker 0.080 CPU / 5,187
switches; GPUFencingThread 0.089 CPU / 5,484 switches. The subsequent 199 Hz,
5 s cpu-clock recording completed with 3,005 samples. Target stops averaged
117.5 microseconds and peaked at 2,627.3 microseconds; profiling is intrusive
and its interval is not a production FPS result. Offline conversion used the
exact archived ELF rather than the default stripped-symbol names.

GPU stacks include command-queue waits, worker notification calls, fence waits,
command processing and macro execution. Kernel leaf sub_380 covers both waits
and notification syscalls; its aggregate must not be called entirely idle.
The worker has many wait stacks, so these data do not establish driver shader
compilation as the current limit. Frequent handoffs and guest CPU read ordering
remain candidates to investigate without bypassing coherence.

The prior exact crash report was collected before launch and symbolized with
its matched ELF: termination occurred 221 s after app startup, GPU thread,
VK_ERROR_OUT_OF_DEVICE_MEMORY, largest free direct block 1 MiB. The verified
call sites include CreateBuffer and staging allocation/recovery while preparing
sparse texture uploads. Stack scanning can retain older call return addresses,
so the full scanned list is not treated as a continuous backtrace.


Scheduler handoff candidate and completed on/off/on trial
------------------------------------------------------

The generated native scheduler already separates the worker's event condition
from queue-drain completion. Its optimized mode wakes the single worker with
notify_one and only broadcasts a drain notification when the queue becomes
empty and a drain waiter exists. The atomic selecting this mode previously
defaulted false. The tested candidate defaulted it true; the existing runtime control
can still select either mode. The retained default returns to false after the
trial below failed to establish a benefit. No GPU accuracy, dependency, submission, fence
or guest-memory coherence policy is changed.

An initial core-source implementation duplicated this generated adaptation;
it was discarded after the native pinned-anchor check rejected it. Its mock
fixture passed sanitizers, but that does not establish a production change or
gain. The tested candidate only changed the mode's default. The existing
actual generated scheduler fixture passed 120,000 FIFO commands in both modes,
queue-drain versus final execution, stopped-worker wake and one drain wake per
burst. The full normal native build, 179 cases / 2,239,932 assertions and
packaging passed. Six maintained core patches still reproduce 132 files.

Candidate eboot:
`766fd504fbde289993e4ade75b158527eafdc372af34523eee20203e2fd2c30b`.
Matched ELF:
`77d73c97643ba0a7ec099051aa15421e00bb5932a437c95a528367ebd544480b`.
Run `1793696847735593185` acquired the shared lease and launched fresh PID 824.
ps5debug verified 32 live main-function bytes against the exact ELF, confirmed
the mapped single-byte handoff atomic at runtime 0x4d78bf8 held 1, and read-back
verified the bounded on/off/on writes. Terra inspected all seven title/phase
captures: rendered palm-settlement, desert-valley, canyon and sandy-road title
scenes, with no obvious major corruption or missing geometry.

Full first-observed counter windows with two-second interval margins:

| Mode | Frames / seconds | FPS | Worst frame | GPU CPU / context switches in subsequent 3 s stat |
| --- | --- | --- | --- | --- |
| On, first | 157 / 10.089246 | 15.561123 | 237.595 ms | 0.418 / 15,128 |
| Off | 241 / 15.032019 | 16.032444 | 358.830 ms | 0.402 / 14,940 |
| On, second | 156 / 10.087091 | 15.465311 | 220.508 ms | 0.403 / 17,886 |

The camera/scenery and memory pressure evolved; these short intervals do not
establish a controlled regression either. No consistent FPS or context-switch
benefit is established. The >40 FPS objective remains unmet. The default-on
source change is therefore removed; the tested alternative remains available
through the existing control for further scoped diagnosis. No native rebuild
is claimed for that restoration: the earlier 7050e969 executable remains the
qualified default-off shader-cache artifact, and future builds must use the
restored source rather than reusing the default-on package.

Matched read-only counter snapshots around phases show driver allocations
between 3,435.34 and 3,827.97 MiB and the app's sampled free-block counter falling
from 1,708 MiB to 576 MiB. These are not simultaneous total-pool snapshots,
and the app's free counter refreshes through its cache callbacks, so this is
not a fresh direct-pool query. They do not
explain every allocation or establish long-term memory stability. This bounded
run completed without a detected core failure; it is not an OOM fix. The native
atomic was restored to its original value 1 and read-back verified before app
shutdown. The original console package files, metadata and controls were
restored and hash verified, with idle confirmed before lease release. All three
per-PID allocation/transcode dump files were absent.

Memory-budget source review found that native generation already expresses
cache usage and budget in the shared-pool callback's domain. The separate
payload's failed AvailableDirectMemorySize call must not be generalized to the
app: its pre-OOM log reported a valid 32 MiB free block. A fixed budget figure
alone therefore does not explain the OOM. No further memory-policy change is
included in this candidate.


Loading presentation ownership for split GPU execution
-----------------------------------------------------

Source review found that the frontend's LoadingTick presented Vulkan loading
frames directly. With split GPU execution, the back thread owns the renderer
and can composite guest frames concurrently. The fix sends loading presentation
through the split queue, captures each HUD snapshot by value and mutates shared
HUD storage only on the renderer owner. Loading completion, start time and frame
count are atomic because the frontend updates them while the renderer reports
the first guest frame. Queued loading work checks completion again on execution
and drops itself after the first guest frame.

This builds on the loading-owner fix in sibling investigation commit bf48c2c;
the local implementation also confines HUD mutation to the owner and removes
races in shared loading counters. It does not enable split execution or change
accuracy settings by default. The actual LoadingTick host fixture passed
AddressSanitizer/UndefinedBehaviorSanitizer and a separate ThreadSanitizer run:
immediate/deferred presentation, owner-only HUD writes, independent queued
snapshots, late-frame cancellation, concurrent guest transition and direct
back-thread invocation. Existing HUD timing checks cover the pacing policy.

The normal native build, 179 cases / 2,239,932 assertions, new fixture and package
passed. Candidate eboot:
`9c17df7405ce61c4c35a4d865aae4bd5c6ece6263fbfcfb99ccbb2571535cc0c`.
Matched ELF:
`e80e9ae4cb89b228cf43d412b461db83f08894352927875ac9a5df078054d8af`.
The driver archive and six maintained core patches remain unchanged.

The two-boot comparison completed under one shared lease. It changed only the
existing per-game Fast GPU preference, off then on, with 1x resolution and
frame generation off. Fast GPU explicitly selects Low GPU accuracy, which
allows the existing split engine path; the normal High path was the control.
Unsafe CPU/DMA, async shaders, skip invalidation and the block list stayed off;
reactive flushing stayed on, as verified by launch logs.

| Run / PID | Setting | Valid frames / seconds | FPS | Worst frame |
| --- | --- | --- | --- | --- |
| 1793698544790593741 / 827 | Fast GPU off, High | 388 / 30.218228 | 12.839932 | 669.777 ms |
| 1793698719421781801 / 829 | Fast GPU on, Low | 550 / 35.231547 | 15.611009 | 420.317 ms |

Only full first-observed windows inside the measured interval with two-second
margins are included. Terra inspected both control captures and all three Low
captures. The control showed rendered autumn-garden title scenes; Low showed
castle and Peach Resort scenes. No obvious major corruption or missing geometry
was visible. Their different scenes prevent a controlled speed or quality
comparison. Small dark marks in the Low end capture could not be classified
from the still. The >40 FPS target remains unmet. This is an accuracy-setting
comparison, not evidence of a driver-only speedup or general Low-accuracy
correctness. Default accuracy is unchanged.

The Low launch log confirmed split execution: engine methods on the second
thread and macros on the first. A subsequent 3.004985 s stat sample measured
3.846 process CPUs / 136,795 context switches, including front GPU 0.354 CPU /
41,190 switches and GPUBack 0.639 CPU / 32,023 switches. After the FPS interval,
a 5 s, 99 Hz cpu-clock profile captured 1,742 samples; target stops averaged
119.2 microseconds and peaked at 676.9 microseconds. Its overhead is excluded
from the FPS windows. Symbols came from the frozen matched ELF.

In that recording, 77.25% of the front GPU thread's sample weight was exclusive
to GpuSplit::Drain. Of GPUBack's 310 samples, 132 were exclusive to BackLoop:
113 at ELF 0x1490872 and 11 at 0x1490879. Matched disassembly places these just
after PAUSE in the 8,192-iteration idle poll, loading the published position and
checking the loop bound. Thus the BackLoop symbol alone must not be treated as
command execution cost. The idle loop and front drain are evidence-backed
candidates for a bounded timing experiment; reducing spins is not assumed to
improve FPS, because parking can add latency. No ordering bypass is authorized
by these samples.

Both runs completed without a detected core failure. All three per-PID dump
files were absent. Package files, metadata and controls were restored and hash
verified after each run, with idle confirmed. The settings file was restored
byte-for-byte after each run, SHA256
`0b0d73fce94f57e88b85cf48a3540ff3513e90500d793aa3f6eca093fa043d05`.
This bounded observation does not establish long-term memory stability.


Explicit GPU-back idle spin experiment
-------------------------------------

A private PS5-only diagnostic added a volatile four-byte atomic, default 8,192
and clamped to 65,536 iterations, to the existing idle PAUSE loop. All queue
publication, consumption, waits and stop ordering stayed unchanged. Seven
patches reproduced 133 core files for that probe; full native build/checks and
packaging passed, including 179 cases / 2,239,932 assertions. Executable:
`4981b7a19aa9a780a856209765eee1e401795454b50394a7d11ac31f66ee39bc`;
matched ELF:
`593209ab8e306dee6838614467e373af5e23b05079433a26fa9a552ba7263136`.
Disassembly verified the runtime load and clamp, including the zero branch.

The first attempt, `1793699595097613168`, stopped before identity or FPS
measurement: startup briefly removed the boot log and FTP returned 550. The
app did reach its native boot trace; this is not a demonstrated driver failure.
Package/settings restoration and idle were verified. The startup-only guard
now tolerates missing/empty boot logs within 60 s, while experiment guards
continue to reject identity changes. A retry used the same frozen artifact.

Run `1793700134950415899`, fresh PID 847, completed an 8,192 / 0 / 8,192 trial
at Low accuracy, Fast GPU on, reactive flushing on, 1x and frame generation off.
Live main bytes matched the ELF; the four-byte atomic at runtime 0x4d81680
started at 8,192, each write was read-back verified, and the original value was
restored before shutdown. Conservative full windows with two-second margins:

| Explicit spins | Frames / seconds | FPS | Worst frame | GPUBack CPU / context switches in subsequent 3 s stat |
| --- | --- | --- | --- | --- |
| 8,192, first | 228 / 15.050065 | 15.149436 | 372.857 ms | 0.540 / 23,051 |
| 0 | 123 / 10.083597 | 12.198028 | 687.214 ms | 0.562 / 96,681 |
| 8,192, restored | 233 / 15.141946 | 15.387718 | 1,040.297 ms | 0.655 / 31,701 |

Terra inspected all seven captures: rendered icy/snowy title scenes and a late
foggy bridge scene, without obvious major corruption or missing geometry.
Camera/scenery and memory pressure changed; this does not establish a
controlled regression. It establishes no benefit from removing the explicit
spin loop, and CPU utilization did not show a consistent reduction. Zero here
removes the explicit loop; the C++ atomic-wait implementation has its own
backoff. The original policy is retained. The diagnostic atomic and seventh
patch were removed from retained source; six patches again reproduce 132
files. The tested probe remains private with its matched source/ELF receipt.
No rebuild is claimed for that restoration; the qualified 9c17df74 loading-fix
artifact remains available and future packaging must rebuild restored source.

All phases completed without a detected core failure. All three per-PID dump
files were absent. Package/metadata/controls and the exact original settings
bytes were restored and verified, idle confirmed, before releasing the lease.
The >40 FPS objective remains unmet.


Reactive flushing off with the qualified loading-fix artifact
-----------------------------------------------------------

Run `1793701678301420614`, fresh PID 868, reused the qualified 9c17df74 eboot /
e80e9ae4 ELF. Fast GPU stayed on (Low accuracy), only reactive flushing was
disabled relative to the earlier Low case. Launch logs verified both settings,
unsafe CPU/DMA off, async shaders off and skip invalidation off. Resolution was
1x and frame generation off. The split log confirmed engine work on the second
thread and macros on the first.

Ten full first-observed windows within the 60 s measured interval, excluding
two-second boundaries, total 577 frames / 50.717545 s: **11.376734 FPS**, worst
501.613 ms. Terra viewed start, end and post-profile images: rendered road,
green-terrain and industrial title scenery with no obvious major corruption
or missing geometry. Viewpoints and nearby objects changed. This differs from
the previous castle/garden scenes, so it establishes no controlled regression
or benefit from the preference. It does not meet >40 FPS; the original
preference remains unchanged.

A later 5 s, 99 Hz cpu-clock recording captured 1,663 samples, target stops
averaging 123.3 microseconds and peaking at 2,863.8 microseconds. Profiling was
after the measured interval and excluded from FPS. Native main/ELF identity
and settings receipts were retained. The run completed without a detected core
failure; all three per-PID dump files were absent. Package files, metadata and
controls were restored and hash verified; exact original preference bytes
were restored, SHA256 0b0d73fc... (the same full hash recorded above), and idle
was confirmed before lease release. No long-term stability claim is made.

Back-thread macro trial (not retained)
-------------------------------------

Run `1793702663932404655`, fresh PID 871, selected the existing back-thread
macro mode on PS5 while retaining queue ordering and the original 8,192-spin
policy. Fast GPU was on (Low accuracy), reactive flushing off, resolution 1x
and frame generation off. The launch log confirmed engine methods on the
second thread without the front-thread macro suffix. The candidate passed full
native checks/package and seven-patch / 133-file reproduction. Its eboot was
`021579402f888b0ac91fd8db76eb25f04033995651d31dfd6a09b3fc4371096a`,
matched ELF
`21314fae53744ac0203b154ce0ef1e4fcb3cc23979c7265625691630de8bcfb3`.

Ten full windows inside the measured interval total **641 frames / 50.442917 s
= 12.707433 FPS**, worst 653.896 ms. Terra inspected start, end and post-profile
captures: rendered farmland, dirt road, hay bales and windmill scenery, with
no obvious major corruption. The views differ from the prior reactive-off
run, so the results establish no controlled benefit from moving macros.
The >40 FPS objective remains unmet.

Subsequent profiles were excluded from the FPS interval. A 3.003 s stat sample
measured front GPU 0.478 CPU / 111,208 context switches and GPUBack 0.675 CPU /
22,816 context switches. In the on-CPU profile, front GPU samples were 69.57%
Drain and 22.17% sched_yield; GPUBack samples were 24.84% BackLoop. A later
99 Hz, 5 s off-CPU recording captured 32,604 samples, with target stops averaging
1,012 microseconds and peaking at 3,698.8 microseconds. Its weights describe
blocked wall time across threads, not CPU utilization. Front GPU off-CPU stacks
were 54.86% HostCommandQueue::PopWait, 28.74% FlushGpuWrittenCommands through
Drain and 9.11% through sched_yield. GPUBack was 31.38% atomic-wait kernel
entry and 20.04% BackLoop; driver queue sequence sleeps accounted for 1.62%.
These observations show substantial queue waiting and draining but do not
justify bypassing GPU-written-list ordering.

The original front-thread macro default was restored; the candidate patch and
artifacts remain private experimental evidence. Maintained patches again
reproduce six patches / 132 files. All three per-PID dump files were absent.
Original package files, metadata, controls and exact preference bytes were
restored and verified (settings SHA256 0b0d73fc..., full hash above); idle was
confirmed before lease release. No long-term stability claim is made.


ASTC opacity scan candidate (initial artifact excluded)
------------------------------------------------------

The computational fast path reviewed from sibling commit `57cf423` reads the
single-partition block mode and endpoint mode directly. Reserved modes keep
the current opaque-error result; void extents and multiple partitions use the
existing parser. This implementation assembles bytes explicitly, avoiding a
host-endian assumption. It does not change format selection or GPU ordering.
Other changes from that sibling commit were not imported with this candidate.

`tools/check-astc-opacity.py` extracts the actual parser and compares it with a
frozen pre-optimization reference. All 131,072 combinations of the first 17
header bits are checked with four randomized payloads each, followed by
200,000 randomized inputs of lengths 0-64 (including partial trailing blocks):
**724,288 comparisons passed** under AddressSanitizer/UndefinedBehaviorSanitizer.
A synthetic complete scan of 100 MiB of opaque single-partition blocks took
0.162899 s with the reference and 0.006924 s with the candidate in an unsanitized
host `-O3` build (23.526x). This is a scanner microbenchmark, not title FPS or a
native-console speedup measurement.

Full normal native checks, **179 cases / 2,239,932 assertions**, the differential
fixture, build and integration package passed. Seven maintained patches now
reproduce 133 core files. The candidate eboot is
`5e5180b52d2f4f7223c69155f78961617aab4468885d1ca45e8b7a10cce69621`,
matched ELF
`c8880ef1df66d251651b62f4aeab5603f711d23da35ef67e93ce21e01ad1e0d9`,
source stamp `eglantine-3140648a668aaebf87bae2dea17512fdca125ba0-780987f1b5d6`.
The initial Low/reactive-off/1x/framegen-off trial failed before measurement.
The artifact also retained an obsolete GPU-split default, as documented below;
it cannot qualify performance of the retained source. The >40 FPS objective
remains unmet.


Descriptor invalidation candidate (initial artifact excluded)
-----------------------------------------------------------

The next candidate replaces full descriptor-read bitset clearing with per-word
generation stamps. `Invalidate` advances a 64-bit generation; the next read of
a word clears that word before interpreting its bits. Generation rollover
clears all stamps before reusing generation one. Table refresh resizes both
vectors and invalidates the generation. This adds one 64-bit stamp per 64
descriptors (about 64 KiB at the table's minimum preallocation), trading a
stamp check per read for constant-time normal invalidation.

Native PS5 builds also omit the validation-only walk that poisons every cached
image-view ID after deletion or scale invalidation, following the optimization
reviewed in sibling `57cf423`. Both graphics and compute descriptor tables still
invalidate, so the first lookup replaces the cached ID even when the guest
TIC is unchanged. Other view-reference removal and validation remain intact.
`EDEN_TEXTURE_CACHE_POISON_IDS=1` restores that walk on PS5; desktop builds keep
the previous poisoning behavior.

The actual descriptor table matched its frozen reference in **1,080,219
comparisons** under AddressSanitizer/UndefinedBehaviorSanitizer, covering
unchanged/new descriptors, bit-word boundaries, growth/shrink, GPU-address
rebasing, repeated invalidation and forced generation rollover. A fixture
extracting the actual `VisitImageView` method exercised graphics and compute
lookups in two channels, deletion with stale per-index IDs, replacement with
unchanged guest descriptors and null views. It passed with native poisoning
off/on and with the unchanged desktop default. Mock memory and view allocation
is used; these fixtures do not establish GPU rendering correctness.

Full normal native checks, **179 cases / 2,239,932 assertions**, build and
integration package passed. Eight maintained patches reproduce 134 core files.
The candidate eboot is
`88bc249bb8f00808dd83ad6f4b98afe1b88d9ef5ef50672b71cdc7236d73ca69`,
matched ELF
`81edb8cf46e49e393d44bba818f1fa36bec29a93debc491ab661c639ddb52122`,
source stamp `eglantine-3140648a668aaebf87bae2dea17512fdca125ba0-b9de4ef6eb59`.
This artifact includes the preceding ASTC scan change. Its initial bounded
Low/reactive-off/1x/framegen-off console trial was rejected by the required
front-macro mode guard. Source/ELF auditing found the obsolete fallback below.
Neither this artifact nor the ASTC-only one qualifies performance of retained
source. The >40 FPS objective remains unmet.


Staging-only command-list wait diagnostic (not retained)
-------------------------------------------------------

Source review found that `Nx2SynchronizeGpuWrittenCommands` drains before it
checks for GPU-written pages. Moving the page check ahead of the wait would
miss pages written/marked by work still queued on the back. The earlier peer
writer classification also omitted executable work and failed during loading.
This diagnostic takes a more conservative approach: only non-macro Maxwell3D
methods selected by its immutable non-executable method mask receive a distinct
staging entry. The consumer only appends these entries to `method_sink`.
Ordinary methods, direct macro writes, multi-method calls and deferred callbacks
all advance the required-effect position. Full drains at other sites remain.

The consumer acknowledges a completed prefix before entering a staging-only
suffix; command-list reads may wait for that prefix. The existing GPU-written
page test and cache flush still follow the effect wait. A private aligned
four-byte native atomic selects full versus prefix waits, defaulting to full.
Counters identify actual shortened waits and trailing words. The bounded
console runner verifies PID/title, main-code bytes, matched symbol size,
writable mapping and readback before toggling 0/1/0, and restores the atomic
and original files/preferences in cleanup. These are diagnostic controls,
not retained public defaults.

A fixture compiles the actual queue, consumer and reset methods with mocked
engines. ASan/UBSan and separate TSan runs passed: stalled staging suffix while
the effect wait returns, full-drain blocking, required ordinary methods, direct
macro writes, multi-method packets including multiple pieces, deferred and heap
callbacks, explicit wrap marker, more than two rings of FIFO traffic, engine
switches and session restart/reset. This establishes the exercised queue
properties with mocks; GPU cache/renderer correctness remains unverified.
Full normal native checks, **179 cases / 2,239,932 assertions**, build and
package passed; the private nine-patch source reproduced 137 files.

Diagnostic eboot:
`0e74fa05593d72a3c9d88e9264898e085ebf43936f10b93316f501529b73ddaa`,
matched ELF:
`272fb2fdad66bdef981a95899185babcfc4ae2eb4e5932ab37c6c47c8b33c953`,
source stamp `eglantine-3140648a668aaebf87bae2dea17512fdca125ba0-2b561656846a`.
The full/prefix/full console comparison completed under the lease at Low
accuracy, reactive flushing off, 1x and frame generation off. The diagnostic
patch/source were archived privately and removed from the retained tree after
freezing this artifact. The public branch still reproduces eight patches /
134 core files and retains full command-list drains. Its console results and their limits follow.


Completed wait trial and source/ELF mismatch audit
-------------------------------------------------

Run `1793706167102802180`, fresh PID 896, confirmed front-thread macros and
all intended launch settings. The same executable's four-byte atomic was
verified and changed 0/1/0, then restored to its exact original zero value.
Full first-observed windows with two-second margins gave:

| Phase | Frames / seconds | FPS | Worst frame |
| --- | --- | --- | --- |
| Full A | 175 / 10.051941 | 17.409573 | 190.189 ms |
| Staging-tail prefix | 168 / 10.027985 | 16.753116 | 149.832 ms |
| Full B | 140 / 10.396478 | 13.466099 | 595.975 ms |

Terra inspected all seven captures. They show rendered title attract scenes,
starting on the coastal industrial road and changing to inland junction,
bridge and hillside scenes. No obvious major foreground corruption or
occlusion problem was visible; some distant track-like pieces in the initial
view cannot be classified from the still. Camera/scene changes prevent a
controlled equivalence or speedup conclusion. Still images do not establish
absence of transient artifacts. The mode-one log at its end capture recorded
509,935 shortened waits and 996,564 trailing words, demonstrating execution of
the proposed path. Subsequent three-second stat samples measured GPU front
0.408 / 0.464 / 0.397 CPU and 69,885 / 18,399 / 51,627 context switches; GPUBack
0.636 / 0.687 / 0.633 CPU. Lower context-switch count does not establish a frame
rate benefit. The optimization remains unretained. All per-PID dump files were
absent, atomic/package/controls/exact preference restoration was verified and
idle confirmed. No >40 FPS or long-term stability result was obtained.

The ASTC-only run `1793705929353227070`, PID 891, restarted before its 800-frame
readiness threshold. Its crash report records GPUBack termination for
VK_ERROR_OUT_OF_DEVICE_MEMORY at **90 seconds**, largest contiguous free block
rounded down to **0 MiB**. Staging allocation failed for 699,064 requested bytes
(1 MiB allocation); releasing 177 completed buffers totaling 3 MiB did not
resolve it, and a subsequent 1,398,128-byte request (2 MiB) failed. Exact ELF
symbolization and preceding call bytes support the CreateBuffer / staging
buffer request / ASTC-to-BCn transcode path; scanned stack entries are not a
continuous unwound backtrace. No screenshot or valid measured window exists.
The original package and preferences were restored, hash verified and idle
confirmed. This is not evidence of a regression caused by the opacity scan.

The descriptor run `1793706047026434222`, PID 894, reached readiness but its
GPU-split log omitted the required front-macro suffix. The guard rejected the
run before capture or measurement, and original files/preferences were restored
and verified. ELF disassembly then found an obsolete **mode '1'** fallback in
both initial cache artifacts: `movb $0x31,%al` at 0x148f5cf in their respective
matched ELFs. Retained source selected **'2'**. Neither build log rebuilt
gpu_split.cpp. The diagnostic's forced recompilation did rebuild it and its
ELF contains `movb $0x32,%al` at 0x148f5ce; its console log confirmed front macros.
Consequently the earlier cache artifacts are excluded as source qualification,
even though their build checks and host algorithm fixtures passed.

The synchronization script preserved restored source timestamps with rsync -a,
which could leave a newer stale Ninja object untouched. It now compares content
and assigns fresh mirror timestamps to changed files. A versioned policy
refreshes legacy mirrors once, including when their source stamp already
matches, while preserving the dependency cache. An actual CMake/Ninja fixture
proves rebuilds for legacy stale objects, same-size tracked and untracked
old-mtime restores, no-op object preservation and deletion semantics. The old
script fails the fixture (executable returns 1 instead of 2); the revised script
passes. The native build now runs that regression gate. A one-time refreshed build completed **1,370 Ninja steps**, normal native checks,
179 cases / 2,239,932 assertions, both cache fixtures and integration packaging.
Its eboot is
`65d9cdb328e0807978022405be273c6c063d690bdd2f457043108c0bf83f0e9b`,
matched ELF
`7a7ab53c2caf7f49782992fff566793c7442e8b13578e33240d09950c98d119b`.
The source stamp remains `...-b9de4ef6eb59`, matching the intended descriptor
candidate source, but the rebuilt ELF now contains fallback `b032` at
0x148f5cf; the private diagnostic atomic is absent. Policy receipt is
`content-mtime-v1`. Its Low/reactive-off/1x/framegen-off console qualification
is queued under the shared lease. Profiler failures outside measurement windows are logged
separately by the private runners and do not substitute for valid FPS/visual
checks. The >40 FPS objective remains unmet.
