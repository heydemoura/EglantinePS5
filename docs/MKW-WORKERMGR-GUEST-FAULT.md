# MKW: guest thread WorkerMgr/Worker1 jumped to an unmapped address

Status: **open, findings only**. Nothing here is fixed yet; this file records what one crash showed
so the investigation can pick up from it.

## The crash

- Build: `main` at fae2715 (PR #75 merged: startup fixes 0034–0036), release `make install`, built
  2026-11-08 03:24.
- Game: Mario Kart World (`0400C3F00006E000`), Vulkan, 1x.
- When: 156 s after the session's log started, about 97 s after the game loaded.
- Message (`stderr.log`):
  `session failed: The game stopped: guest thread WorkerMgr/Worker1(?) jumped to an unmapped address.`
- No `crash-*.txt` was written. That is expected: the host didn't crash. The guest faulted, and
  Eden stopped the session and went back to the launcher. (The newest crash report on the
  console, `crash-20261107-221508`, is an older GPUBack syncpoint crash from before #75.)

## What the logs show, in order

1. **An index draw with impossible numbers** (`stderr.log`):
   ```
   MKW_INDEX_SPAN begin=2116615000 end=0 device=10816000 first=1482134333 count=1477923631 element=2 address_size=3919491072 draw_size=1625148632 chosen=1625148632
   MKW_BUFFER_MERGE requested_begin=10810000 wanted=1625227264 merged_begin=10810000 merged_end=71600000 size=1625227264 overlaps=139 leap=1
   radv/ps5: 1550 MiB in 775 pieces of 2 MiB (no run that long in the pool)
   ```
   About 1.48 billion 16-bit indices, the first index at 1.48 billion, and an end address of 0. The
   buffer cache merged 139 buffers into one 1.55 GiB buffer, which RADV could only build from
   2 MiB pieces (tools/patch-radv-pieces.py).
2. **Memory recovery under pressure**:
   ```
   EDEN_CRAM_PACK recover asked=1552MiB packed=1288MiB
   radv/ps5: 1536 MiB in 768 pieces of 2 MiB (no run that long in the pool)
   EDEN_STAGING_EXACT 1536 MiB made for a transfer of 1524 MiB, not the 2048 MiB of the next power of two (1 so far)
   ```
   The guest memory service packed 1,288 MiB of cold guest slots to make room for a 1.5 GiB
   staging buffer for the same draw. The same `MKW_INDEX_SPAN` line repeats right after.
3. **The guest reads a pointer that is data** (`eden_log.txt`, 156.205–156.219 s):
   ```
   Unmapped Read8 @ 0x7ddd0b3d7ddd0b4d (#0) ... (#7)
   MemoryRead: Unmapped read 0x7ddd0b3d7ddd0b4d (8 bytes) at pc 0x80cb4bd4 lr 0x80cb4870 sp 0x10fbf7f740
   ExceptionRaised: Cannot execute instruction at unmapped address 0x00000000000000
   Backtrace @ PC=0000000000000004 (callers 0x80cb4a68, 0x80af3204, 0x80aec880, 0x80ee1848,
   0x81636220, 0x830229ec, nnEcli+0xa59630, nnEcli+0xa60b78)
   ```
   `0x7ddd0b3d7ddd0b4d` is not an address. The same 32-bit value `0x7DDD0B3D` (a huge float) sits in
   V0 and V2, so float data is where the code expects a pointer. The read fails, the code calls
   through the null result, and the thread lands at 0.

Earlier in the same session, at 87.6 s, there were 16 `Unmapped Device ReadBlock` errors
(`core/device_memory_manager.inc:533`, size 69632, at device addresses `0x706d62000` and
`0x160a72000`). The GPU read guest memory that wasn't mapped.

## Reading

- The draw with impossible numbers and the garbage pointer both point to guest data being wrong
  before the fault, not to a bug in the faulting code.
- The 1.5 GiB buffer and the 1.3 GiB pack pass came right before the fault, which makes them
  suspects (for example a slot unpacked with the wrong contents, or the guest touching a slot
  while it is packed). The logs only show the order of events, not the cause.
- Not related to the startup fixes in #75: this is well past startup, on a game thread, not
  GPUBack or VulkanWorker.

The previous session's log (`stderr.prev.log`) has a different guest fault, `Prepare Thread
jumped to an unmapped address`, which ended in an automatic game restart. It hasn't been looked
at.

## Next steps

1. Run with packing off (`EDEN_CRAM_PACK=0`, read once in `src/guest_memory_service.inc` PackEnabled) and see whether the
   WorkerMgr fault and the impossible draw still happen.
2. Diagnostic build that logs, for an index draw over a size limit (say 256 MiB): the
   index buffer address and count registers, the engine method that set them, and which guest
   slots behind that memory were packed or unpacked recently.
3. Refuse or clamp index draws whose span is impossible (`end=0`, count over the guest buffer
   size) instead of building a 1.5 GiB buffer. That avoids the memory pressure even if the source
   isn't fixed yet.
4. Map guest pc `0x80cb4bd4` / lr `0x80cb4870` to the game's code to see which structure held
   the pointer.

## Where the logs are

On the console (until they rotate): `/data/eglantineps5/user/log/eden_log.txt` and
`/data/eglantineps5/logs/stderr.log`. Copies on the dev machine: `.local/crash-latest/`
(`eden_log.txt` 78 MB, `stderr.log`, `stderr.prev.log`, `heap.prev.log`, boot traces).
