#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A freed buffer keeps its memory until the GPU has run what was submitted before.

A buffer or sparse range was unmapped and its direct memory released the moment it was
destroyed. The GPU runs submissions behind the CPU, so a read it had still to make of that memory
hits an unmapped page: the GPU faults and the process is ended with no signal and no crash report.
Ocarina of Time died that way two seconds after its opening movie, when the first scene's loading
freed the movie's buffers, in every run.

Upstream PS5_Mesa f5610fe270 (Mihawk) holds the freed memory until the submissions made before the
free have run. This is that change, adapted to EglantinePS5's memory trim (patch-radv-trim.py):
trim keeps a table of pointers to each buffer's struct radv_ps5_memory, so the whole buffer object
is held rather than a copy of its memory struct, and its address stays the one trim knows until
radv_ps5_memory_free untracks it. An allocation that fails waits for the GPU to finish with held
memory and tries again before reporting out of memory. Applied after patch-radv-trim.py. Patches
the build export, never the Mesa checkout.
"""
from pathlib import Path
import hashlib
import sys

root = Path(__file__).resolve().parents[1]
fingerprint = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
if sys.argv[1:2] == ['--check']:
    stamp = Path(sys.argv[2])
    raise SystemExit(0 if stamp.is_file() and stamp.read_text().strip() == fingerprint else 1)
if sys.argv[1:2] == ['--digest']:
    print(fingerprint)
    raise SystemExit(0)
source = (Path(sys.argv[1]) if len(sys.argv) > 1 else
          root.parent / 'mihawk-vulkan-review/.deps/work/radv-src')
MARK = 'EglantinePS5 retire'

RETIRE = r'''

/* ''' + MARK + r''' (tools/patch-radv-retire.py, from upstream f5610fe270): a buffer freed
 * while the GPU may still run submissions made before the free is held, whole, until they have
 * run, as the amdgpu kernel keeps a freed buffer's pages until its fences signal. Unmapped at
 * once, a read the GPU had still to make faults it and the title is ended. The buffer object
 * itself is held so that its memory struct keeps the address the memory trim tracks. */
struct radv_ps5_retired {
   struct radv_ps5_bo *bo;
   uint64_t seq;
};
static simple_mtx_t radv_ps5_retired_lock = SIMPLE_MTX_INITIALIZER;
static struct util_dynarray radv_ps5_retired;

static void
radv_ps5_bo_release(struct radv_ps5_bo *bo)
{
   if (bo->sparse)
      radv_ps5_vrange_release(&bo->memory);
   else
      radv_ps5_memory_free(&bo->memory);
   free(bo);
}

/* Gives back what the GPU has finished with; the newest submission the rest waits for, or 0 if
 * nothing is held. */
static uint64_t
radv_ps5_retired_free(struct radv_ps5_queue *queue)
{
   simple_mtx_lock(&radv_ps5_retired_lock);
   const uint64_t completed = radv_ps5_queue_poll(queue);
   uint64_t newest = 0;
   unsigned kept = 0;
   util_dynarray_foreach (&radv_ps5_retired, struct radv_ps5_retired, entry) {
      if (entry->seq <= completed) {
         radv_ps5_bo_release(entry->bo);
      } else {
         newest = MAX2(newest, entry->seq);
         *util_dynarray_element(&radv_ps5_retired, struct radv_ps5_retired, kept++) = *entry;
      }
   }
   radv_ps5_retired.size = kept * sizeof(struct radv_ps5_retired);
   simple_mtx_unlock(&radv_ps5_retired_lock);
   return newest;
}

static void
radv_ps5_bo_retire(struct radv_ps5_queue *queue, struct radv_ps5_bo *bo)
{
   if (!queue) {
      radv_ps5_bo_release(bo);
      return;
   }
   const uint64_t seq = p_atomic_read(&queue->submitted_seq);
   if (seq <= radv_ps5_queue_poll(queue)) {
      radv_ps5_bo_release(bo);
   } else {
      simple_mtx_lock(&radv_ps5_retired_lock);
      const struct radv_ps5_retired entry = {.bo = bo, .seq = seq};
      util_dynarray_append(&radv_ps5_retired, entry);
      simple_mtx_unlock(&radv_ps5_retired_lock);
   }
   radv_ps5_retired_free(queue);
}

/* After an allocation failed: waits for the GPU to finish with the memory held for it and gives
 * it back; false if nothing was held. */
static bool
radv_ps5_retired_wait(struct radv_ps5_queue *queue)
{
   if (!queue)
      return false;
   const uint64_t newest = radv_ps5_retired_free(queue);
   if (!newest || !radv_ps5_queue_wait_seq(queue, newest, OS_TIMEOUT_INFINITE))
      return false;
   radv_ps5_retired_free(queue);
   return true;
}'''

edits = {
    'src/amd/vulkan/winsys/ps5/radv_ps5_bo.c': [
        ('#include "util/u_atomic.h"\n',
         '#include "util/u_atomic.h"\n#include "util/simple_mtx.h"\n#include "util/u_dynarray.h"\n'),
        ('uint64_t radv_ps5_allocated_bytes;\n',
         'uint64_t radv_ps5_allocated_bytes;\n' + RETIRE + '\n'),
        ('''      if (!radv_ps5_vrange_reserve(size, flags & RADEON_FLAG_32BIT,
                                   (flags & RADEON_FLAG_REPLAYABLE) || replay_address, replay_address,
                                   &bo->memory)) {
''', '''      bool reserved;
      do {
         reserved = radv_ps5_vrange_reserve(size, flags & RADEON_FLAG_32BIT,
                                            (flags & RADEON_FLAG_REPLAYABLE) || replay_address, replay_address,
                                            &bo->memory);
      } while (!reserved && radv_ps5_retired_wait(ws->queue));
      if (!reserved) {
'''),
        ('''   if (replayable ? !radv_ps5_memory_alloc_replayable(size, alignment, window32, replay_address, &bo->memory)
                  : !radv_ps5_memory_alloc(size, alignment, window32, &bo->memory)) {
''', '''   bool allocated;
   do {
      allocated = replayable ? radv_ps5_memory_alloc_replayable(size, alignment, window32, replay_address, &bo->memory)
                             : radv_ps5_memory_alloc(size, alignment, window32, &bo->memory);
   } while (!allocated && radv_ps5_retired_wait(ws->queue));
   if (!allocated) {
'''),
        ('''   if (bo->sparse) {
      radv_ps5_vrange_release(&bo->memory);
      free(bo);
      return;
   }
''', '''   if (bo->sparse) {
      radv_ps5_bo_retire(ws->queue, bo);
      return;
   }
'''),
        ('''      radv_ps5_memory_unmap_at(bo->placed, bo->memory.bytes, false);
   radv_ps5_memory_free(&bo->memory);
   free(bo);
}
''', '''      radv_ps5_memory_unmap_at(bo->placed, bo->memory.bytes, false);
   radv_ps5_bo_retire(ws->queue, bo);
}
'''),
    ],
}

changed = False
for relative, replacements in edits.items():
    path = source / relative
    text = path.read_text()
    if MARK in text:
        continue
    for old, new in replacements:
        if old not in text and new in text:
            continue
        if text.count(old) != 1:
            raise SystemExit(f'{relative}: pinned anchor changed ({text.count(old)}x): {old.strip().splitlines()[0]!r}')
        text = text.replace(old, new)
    path.write_text(text)
    changed = True
if changed:
    receipt = root.parent / 'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if source.resolve() == (root.parent / 'mihawk-vulkan-review/.deps/work/radv-src').resolve() and receipt.exists():
        receipt.replace(receipt.with_suffix('.base.txt'))
print('RADV retire: a freed buffer keeps its memory until the GPU has run earlier submissions')
