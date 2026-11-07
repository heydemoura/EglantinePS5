#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Driver memory from 2 MiB pieces when the console's pool has no span in one piece.

The PS5 winsys takes each buffer object as one run of direct memory. The pool it shares with the
guest's memory and Eden's heap fragments over a session. On Mario Kart World, with the guest
memory service packing cold slots (src/guest_memory_service.inc), ~650 MiB was free in 2 MiB
blocks while the largest free run was 24 MiB, and the driver's 64 MiB VMA block failed:
VK_ERROR_OUT_OF_DEVICE_MEMORY. When the single run is refused, a buffer outside the 32-bit window now
takes its device-region granules and maps one 2 MiB piece of direct memory at each, in order. The
GPU address is the CPU mapping, as before, so nothing else changes. Freeing, sparse binding and
mapping at another address go piece by piece. Patches the build export, never the Mesa checkout.
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
MARK = 'EglantinePS5 memory pieces'

PIECES = r'''
/* ''' + MARK + r''': a buffer from 2 MiB pieces of direct memory, mapped in order at its
 * device-region granules, for when the pool has no run as long as the buffer. Each piece is
 * mapped at the address asked for or not at all (never over another mapping). */
/* The app's: gives memory back when the pool has none (EglantinePS5, absent elsewhere). */
extern size_t eden_memory_recover(size_t bytes) __attribute__((weak));

static bool
radv_ps5_memory_alloc_pieces(uint64_t bytes, uint64_t alignment, struct radv_ps5_memory *out)
{
   const uint32_t granules = (uint32_t)DIV_ROUND_UP(bytes, RADV_PS5_LARGE_BYTES);
   int64_t *const pieces = calloc(granules, sizeof(*pieces));
   if (!pieces)
      return false;
   const uint32_t granule =
      radv_ps5_granules_take(&radv_ps5_region, granules, (uint32_t)MAX2(alignment / RADV_PS5_LARGE_BYTES, 1));
   if (granule == UINT32_MAX) {
      free(pieces);
      return false;
   }
   uint8_t *const at = (uint8_t *)(uintptr_t)(RADV_PS5_REGION_BASE + (uint64_t)granule * RADV_PS5_LARGE_BYTES);
   uint32_t done = 0;
   for (; done < granules; ++done) {
      int64_t physical = -1;
      if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), RADV_PS5_LARGE_BYTES, RADV_PS5_LARGE_BYTES,
                                        RADV_PS5_DIRECT_TYPE, &physical) != 0)
         break;
      void *const want = at + (uint64_t)done * RADV_PS5_LARGE_BYTES;
      void *address = want;
      const int32_t result =
         sceKernelMapDirectMemory(&address, RADV_PS5_LARGE_BYTES, RADV_PS5_PROTECTION, 0, physical, RADV_PS5_LARGE_BYTES);
      if (result != 0 || address != want) {
         if (result == 0 && address != NULL)
            sceKernelMunmap(address, RADV_PS5_LARGE_BYTES);
         sceKernelReleaseDirectMemory(physical, RADV_PS5_LARGE_BYTES);
         break;
      }
      pieces[done] = physical;
   }
   if (done != granules) {
      for (uint32_t i = 0; i < done; ++i) {
         sceKernelMunmap(at + (uint64_t)i * RADV_PS5_LARGE_BYTES, RADV_PS5_LARGE_BYTES);
         sceKernelReleaseDirectMemory(pieces[i], RADV_PS5_LARGE_BYTES);
      }
      radv_ps5_granules_give(&radv_ps5_region, granule, granules);
      free(pieces);
      return false;
   }
   *out = (struct radv_ps5_memory){
      .cpu = at,
      .bytes = (uint64_t)granules * RADV_PS5_LARGE_BYTES,
      .physical = -1,
      .granule = granule,
      .granules = granules,
      .pieces = pieces,
   };
   static uint32_t told;
   if (told++ < 4)
      fprintf(stderr, "radv/ps5: %" PRIu64 " MiB in %u pieces of 2 MiB (no run that long in the pool)\n",
              out->bytes >> 20, granules);
   return true;
}

/* Maps [offset, offset + bytes) of a pieced buffer at `at`, piece by piece. */
static bool
radv_ps5_pieces_map(const struct radv_ps5_memory *memory, uint8_t *at, uint64_t bytes, uint64_t offset, int protection)
{
   for (uint64_t done = 0; done < bytes;) {
      const uint64_t inner = (offset + done) % RADV_PS5_LARGE_BYTES;
      const uint64_t length = MIN2(bytes - done, RADV_PS5_LARGE_BYTES - inner);
      void *address = at + done;
      const int32_t result =
         sceKernelMapDirectMemory(&address, length, protection, PS5_KERNEL_MAP_FIXED,
                                  memory->pieces[(offset + done) / RADV_PS5_LARGE_BYTES] + (int64_t)inner,
                                  RADV_PS5_PAGE_BYTES);
      if (result != 0 || address != at + done)
         return false;
      done += length;
   }
   return true;
}

bool
radv_ps5_memory_alloc(uint64_t bytes, uint64_t alignment, bool window32, struct radv_ps5_memory *out)
'''

edits = {
    'src/amd/vulkan/winsys/ps5/radv_ps5_platform.h': [(
        '''   uint32_t granules;
   bool window_replay;
};
''',
        '''   uint32_t granules;
   bool window_replay;
   /* The 2 MiB pieces behind it when the pool had no run that long (physical is -1;
    * ''' + MARK + '''). */
   int64_t *pieces;
};
''')],
    'src/amd/vulkan/winsys/ps5/radv_ps5_platform.c': [
        ('''      return 0;
   return available;
}

bool
radv_ps5_memory_alloc(uint64_t bytes, uint64_t alignment, bool window32, struct radv_ps5_memory *out)
''', '''      return 0;
   return available;
}
''' + PIECES),
        ('''   int64_t physical = -1;
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, alignment,
                                     RADV_PS5_DIRECT_TYPE, &physical) != 0)
      return false;

   /* A window buffer goes where''',
         '''   int64_t physical = -1;
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, alignment,
                                     RADV_PS5_DIRECT_TYPE, &physical) != 0)
   {
      /* No run that long: pieces; else the app gives memory back (eden_memory_recover packs cold
       * guest memory) and the run, or pieces, are asked for once more. */
      if (!window32 && bytes >= RADV_PS5_LARGE_BYTES && radv_ps5_memory_alloc_pieces(bytes, alignment, out))
         return true;
      if (!eden_memory_recover || eden_memory_recover(bytes + (UINT64_C(16) << 20)) == 0)
         return false;
      if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, alignment,
                                        RADV_PS5_DIRECT_TYPE, &physical) != 0)
         return !window32 && bytes >= RADV_PS5_LARGE_BYTES && radv_ps5_memory_alloc_pieces(bytes, alignment, out);
   }

   /* A window buffer goes where'''),
        ('''   const int32_t unmapped = sceKernelMunmap(memory->cpu, memory->bytes);
   if (unmapped != 0)
      fprintf(stderr, "radv/ps5: sceKernelMunmap(%p, %" PRIu64 ") failed: 0x%08x\\n", (void *)memory->cpu,
              memory->bytes, (unsigned)unmapped);
   radv_ps5_granules_give(&radv_ps5_region, memory->granule, memory->granules);
   if (memory->physical >= 0)
      sceKernelReleaseDirectMemory(memory->physical, memory->bytes);
''',
         '''   const int32_t unmapped = sceKernelMunmap(memory->cpu, memory->bytes);
   if (unmapped != 0)
      fprintf(stderr, "radv/ps5: sceKernelMunmap(%p, %" PRIu64 ") failed: 0x%08x\\n", (void *)memory->cpu,
              memory->bytes, (unsigned)unmapped);
   radv_ps5_granules_give(&radv_ps5_region, memory->granule, memory->granules);
   if (memory->physical >= 0)
      sceKernelReleaseDirectMemory(memory->physical, memory->bytes);
   if (memory->pieces) {
      for (uint64_t i = 0; i < memory->bytes / RADV_PS5_LARGE_BYTES; ++i)
         sceKernelReleaseDirectMemory(memory->pieces[i], RADV_PS5_LARGE_BYTES);
      free(memory->pieces);
   }
'''),
        ('''   if (memory->physical < 0 || offset > memory->bytes || bytes > memory->bytes - offset ||
       (uintptr_t)at % RADV_PS5_PAGE_BYTES || offset % RADV_PS5_PAGE_BYTES || bytes % RADV_PS5_PAGE_BYTES)
      return false;
   return radv_ps5_map_fixed(at, bytes, memory->physical + (int64_t)offset);''',
         '''   if ((memory->physical < 0 && !memory->pieces) || offset > memory->bytes || bytes > memory->bytes - offset ||
       (uintptr_t)at % RADV_PS5_PAGE_BYTES || offset % RADV_PS5_PAGE_BYTES || bytes % RADV_PS5_PAGE_BYTES)
      return false;
   if (memory->pieces)
      return radv_ps5_pieces_map(memory, at, bytes, offset, RADV_PS5_PROTECTION);
   return radv_ps5_map_fixed(at, bytes, memory->physical + (int64_t)offset);'''),
        ('''   if (memory->physical < 0)
      return false;
   void *at = address;''',
         '''   if (memory->pieces)
      return radv_ps5_pieces_map(memory, address, memory->bytes, 0,
                                 PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE);
   if (memory->physical < 0)
      return false;
   void *at = address;'''),
    ],
}
includes = {'src/amd/vulkan/winsys/ps5/radv_ps5_platform.c': ['#include <inttypes.h>', '#include <stdio.h>',
                                                               '#include <stdlib.h>']}

changed = False
for relative, replacements in edits.items():
    path = source / relative
    text = path.read_text()
    if MARK in text:
        continue
    for old, new in replacements:
        if text.count(old) != 1:
            raise SystemExit(f'{relative}: pinned anchor changed: {old.splitlines()[1] if old.startswith(chr(10)) else old.splitlines()[0]!r}')
        text = text.replace(old, new)
    for include in includes.get(relative, []):
        if include not in text:
            first = text.index('#include')
            text = text[:first] + include + '\n' + text[first:]
    path.write_text(text)
    changed = True
if changed:
    receipt = root.parent / 'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if source.resolve() == (root.parent / 'mihawk-vulkan-review/.deps/work/radv-src').resolve() and receipt.exists():
        receipt.replace(receipt.with_suffix('.base.txt'))
print('RADV memory pieces: buffers from 2 MiB pieces, and memory asked back from the app')
