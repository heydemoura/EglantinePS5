#!/usr/bin/env python3
"""Apply the pinned Eden display-lifetime adaptation to the Mesa build export."""
from pathlib import Path
import hashlib
import sys

root = Path(__file__).resolve().parents[1]
source = root.parent/'mihawk-mesa-review/src/vulkan/wsi/wsi_common_videoout.c'
target = root.parent/'mihawk-vulkan-review/.deps/work/radv-src/src/vulkan/wsi/wsi_common_videoout.c'
text = source.read_text()

def replace(old, new):
    global text
    assert text.count(old) == 1, f'Pinned WSI anchor changed: {old[:70]}'
    text = text.replace(old, new)

replace('   bool opened;\n', '   unsigned owners;\n   bool closing, stop;\n   bool opened;\n')
# A set of framebuffers per output size: each remembers the memory to give back.
replace('      uint8_t *buffers;\n   } sets[VIDEOOUT_SIZES];',
        '      uint8_t *buffers;\n      int64_t physical;\n      size_t bytes;\n   } sets[VIDEOOUT_SIZES];')
replace('static int\nsceVideoOutSetFlipRate', 'static int\nsceVideoOutClose(int32_t handle)\n{\n   return 0;\n}\n\nstatic int\nsceVideoOutSetFlipRate')
replace('   uint8_t *buffers = NULL;\n#if defined(__PROSPERO__)\n   int64_t physical = -1;',
        '   uint8_t *buffers = NULL;\n   int64_t physical = -1;\n#if defined(__PROSPERO__)')
replace('   out->sets[index] = (struct wsi_videoout_set){.size = size, .buffers = buffers};',
        '   out->sets[index] = (struct wsi_videoout_set){.size = size, .buffers = buffers, .physical = physical,\n'
        '                                                .bytes = bytes};')
replace('      while (out->queue_count == 0)\n         cnd_wait(&out->changed, &out->lock);',
        '      while (out->queue_count == 0 && !out->stop)\n         cnd_wait(&out->changed, &out->lock);\n      if (out->queue_count == 0 && out->stop) {\n         mtx_unlock(&out->lock);\n         return 0;\n      }')
replace('VkResult\nwsi_display_init_wsi',
        (root/'tools/radv-videoout-lifecycle.inc').read_text()+'\nVkResult\nwsi_display_init_wsi')
# 120 Hz: the app asks for it per game session (headless/display_refresh.h). The pinned source
# decides from the package's param.json, which the app cannot read once it has left the sandbox,
# and for the whole run; the package declares the capability (tools/package-headless-native.sh).
declares = 'videoout_title_declares_high_frame_rate(void)\n{\n'
assert text.count(declares) == 1, 'Pinned WSI anchor changed: the 120 Hz declaration'
body_start = text.index(declares) + len(declares)
body_end = text.index('\n}\n', body_start)
assert 'param.json' in text[body_start:body_end], 'Pinned WSI anchor changed: the 120 Hz declaration reads param.json'
text = (text[:body_start] + '   const char *const asked = getenv("EDEN_VIDEOOUT_120HZ");\n'
        '   return asked != NULL && asked[0] == \'1\';' + text[body_end:])
# The pinned source takes the 120 Hz output back unless a vblank then comes about every 8.3 ms.
# On a display with a variable refresh rate that is wrong: before the first frame is shown it
# idles at the bottom of its range (a vblank every 20.87 ms, 48 Hz, in two reports), the output
# went back to 59.94 Hz, and a game could not have 120 Hz at all. Only a display that stayed at
# 59.94 Hz (a vblank about every 16.7 ms) gives the mode back.
replace('         if (period > VIDEOOUT_HIGH_REFRESH_LIMIT_NS) {',
        '         if (period > UINT64_C(15000000) && period < UINT64_C(18500000)) {')
# Say so when the display follows, as the pinned source does when it does not.
replace('''                    (double)period / 1e6);
         }
      } else {''', '''                    (double)period / 1e6);
         } else {
            fprintf(stderr, "wsi/videoout: 119.88 Hz output, a vblank every %.3f ms\\n", (double)period / 1e6);
         }
      } else {''')
replace('   wsi->base.get_support =', '   videoout_ref();\n\n   wsi->base.get_support =')
replace('   if (wsi)\n      vk_free(alloc, wsi);',
        '   if (wsi) {\n      videoout_unref();\n      vk_free(alloc, wsi);\n      wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY] = NULL;\n   }')
checking = sys.argv[1:2] == ['--check']
if checking:
    # Is the driver that was built (its recorded display source, EDEN_WSI_SHA256) this adaptation?
    # The other adapted files are checked below against the export the driver was built from.
    recorded = Path(sys.argv[2]).read_text().split()[0] if Path(sys.argv[2]).is_file() else ''
    check_ok = recorded == hashlib.sha256(text.encode()).hexdigest()
changed = not checking and (not target.exists() or target.read_text() != text)
if checking:
    pass
elif changed:
    target.write_text(text)
    # Upstream caches the whole build by pin. Invalidate only that receipt;
    # Ninja retains all unchanged objects and rebuilds the patched WSI.
    provenance = root.parent/'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if provenance.exists():
        provenance.replace(provenance.with_suffix('.base.txt'))
if not checking:
    print(f'RADV WSI adaptation changed={changed} sha256={hashlib.sha256(text.encode()).hexdigest()}')

# Device memory from 2 MiB pieces (tools/radv-scatter-memory.inc): a pool cut into 2 MiB holes (the
# guest memory's pager gives slots back one by one) has no contiguous range for a 64 MiB VMA block
# while gigabytes are free. The GPU needs a contiguous address, not contiguous memory.
_MEMORY_ALLOC = ('bool\nradv_ps5_memory_alloc(uint64_t bytes, uint64_t alignment, bool window32, struct radv_ps5_memory *out)\n'
                 '{\n   *out = (struct radv_ps5_memory){.physical = -1};\n   bytes = align64(MAX2(bytes, 1), RADV_PS5_PAGE_BYTES);\n'
                 '   alignment = MAX2(alignment, PS5_KERNEL_DIRECT_ALIGNMENT);')
RADV_SCATTER = [
    ('#include <assert.h>\n', '#include <assert.h>\n#include <stdarg.h>\n'),
    (_MEMORY_ALLOC, (root/'tools/radv-scatter-memory.inc').read_text() + '\n' + _MEMORY_ALLOC),
    ("""   int64_t physical = -1;
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, alignment,
                                     RADV_PS5_DIRECT_TYPE, &physical) != 0)
      return false;
""", """   int64_t physical = -1;
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, alignment,
                                     RADV_PS5_DIRECT_TYPE, &physical) != 0) {
      /* No contiguous range: pieces, in granules of the device-memory region. */
      static unsigned refusals_noted;
      if (refusals_noted++ < 32)
         radv_ps5_note("radv/ps5: no contiguous %" PRIu64 " KiB (alignment %" PRIu64 " KiB, window32 %d) in the pool\\n",
                       bytes >> 10, alignment >> 10, window32);
      if (window32 || !radv_ps5_scatter_enabled() || bytes <= RADV_PS5_LARGE_BYTES)
         return false;
      const uint32_t want = (uint32_t)DIV_ROUND_UP(bytes, RADV_PS5_LARGE_BYTES);
      const uint32_t first = radv_ps5_granules_take(&radv_ps5_region, want,
                                                    (uint32_t)MAX2(alignment / RADV_PS5_LARGE_BYTES, 1));
      if (first == UINT32_MAX)
         return false;
      uint8_t *const va = (uint8_t *)(uintptr_t)(RADV_PS5_REGION_BASE + (uint64_t)first * RADV_PS5_LARGE_BYTES);
      int64_t tag = -1;
      if (!radv_ps5_scatter_fill(va, bytes, &tag)) {
         radv_ps5_granules_give(&radv_ps5_region, first, want);
         return false;
      }
      *out = (struct radv_ps5_memory){.cpu = va, .bytes = bytes, .physical = tag, .granule = first, .granules = want};
      return true;
   }
"""),
    ("""   int64_t physical = -1;
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, alignment, RADV_PS5_DIRECT_TYPE,
                                     &physical) != 0) {
      radv_ps5_granules_give(&radv_ps5_region, granule, granules);
      return false;
   }
""", """   int64_t physical = -1;
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, alignment, RADV_PS5_DIRECT_TYPE,
                                     &physical) != 0) {
      uint8_t *const va = (uint8_t *)(uintptr_t)(RADV_PS5_REGION_BASE + (uint64_t)granule * RADV_PS5_LARGE_BYTES);
      int64_t tag = -1;
      if (radv_ps5_scatter_fill(va, bytes, &tag)) {
         *out = (struct radv_ps5_memory){
            .cpu = va, .bytes = bytes, .physical = tag, .granule = granule, .granules = granules};
         return true;
      }
      radv_ps5_granules_give(&radv_ps5_region, granule, granules);
      return false;
   }
"""),
    ("""   if (memory->physical < 0 || offset > memory->bytes || bytes > memory->bytes - offset ||
       (uintptr_t)at % RADV_PS5_PAGE_BYTES || offset % RADV_PS5_PAGE_BYTES || bytes % RADV_PS5_PAGE_BYTES)
      return false;
""", """   if (radv_ps5_scatter_of(memory->physical)) {
      if ((uintptr_t)at % RADV_PS5_PAGE_BYTES || offset % RADV_PS5_PAGE_BYTES || bytes % RADV_PS5_PAGE_BYTES)
         return false;
      return radv_ps5_scatter_map(at, bytes, memory->physical, offset, RADV_PS5_PROTECTION);
   }
   if (memory->physical < 0 || offset > memory->bytes || bytes > memory->bytes - offset ||
       (uintptr_t)at % RADV_PS5_PAGE_BYTES || offset % RADV_PS5_PAGE_BYTES || bytes % RADV_PS5_PAGE_BYTES)
      return false;
"""),
    ("""   if (memory->physical >= 0)
      sceKernelReleaseDirectMemory(memory->physical, memory->bytes);
   *memory = (struct radv_ps5_memory){.physical = -1};
}
""", """   if (memory->physical >= 0)
      sceKernelReleaseDirectMemory(memory->physical, memory->bytes);
   else
      radv_ps5_scatter_release(memory->physical);
   *memory = (struct radv_ps5_memory){.physical = -1};
}
"""),
    ("""radv_ps5_memory_map_at(const struct radv_ps5_memory *memory, void *address)
{
   if (memory->physical < 0)
      return false;
""", """radv_ps5_memory_map_at(const struct radv_ps5_memory *memory, void *address)
{
   if (radv_ps5_scatter_of(memory->physical)) {
      return radv_ps5_scatter_map(address, memory->bytes, memory->physical, 0,
                                  PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE);
   }
   if (memory->physical < 0)
      return false;
"""),
]

# Preserve the reason behind RADV's successful-but-empty device enumeration.
# These startup-only diagnostics avoid repeated full game runs for each stage.
for relative, replacements in {
    # Restore the pinned upload path: explicit CPU flush did not affect HUD corruption.
    'src/amd/vulkan/radv_cmd_buffer.c': [],
    'src/amd/vulkan/radv_physical_device.c': [
        ('   if (result == VK_ERROR_INCOMPATIBLE_DRIVER || result == VK_ERROR_INITIALIZATION_FAILED)\n      return VK_SUCCESS;',
         '   if (result != VK_SUCCESS) fprintf(stderr, "radv/ps5: physical device creation result=%d\\n", result);\n   if (result == VK_ERROR_INCOMPATIBLE_DRIVER || result == VK_ERROR_INITIALIZATION_FAILED)\n      return VK_SUCCESS;'),
        ('   if (!pdev->addrlib) {', '   if (!pdev->addrlib) {\n      fprintf(stderr, "radv/ps5: addrlib creation failed\\n");'),
        # The PS5 has one heap of driver memory. Counted as both VRAM and visible VRAM, the APU
        # budget added it twice, and a title whose driver memory neared half the heap saw no
        # budget left while the pool still had room: VMA refused allocations (Mario Kart World).
        ('   heap_info->allocated_vram = driver;\n   heap_info->allocated_vram_vis = driver;',
         '   /* EglantinePS5: one heap of driver memory, counted once (the APU budget\n'
         '    * below adds allocated_vram and allocated_vram_vis). */\n'
         '   heap_info->allocated_vram = 0;\n   heap_info->allocated_vram_vis = driver;'),
    ],
    'src/amd/vulkan/winsys/ps5/radv_ps5_winsys.c': [
        ('   if (!radv_ps5_platform_init())\n      return;', '   if (!radv_ps5_platform_init()) {\n      fprintf(stderr, "radv/ps5: queue platform initialization failed\\n");\n      return;\n   }'),
        ('   if (!radv_ps5_memory_alloc(RADV_PS5_RING_BYTES, RADV_PS5_LARGE_BYTES, true, &queue->ring))\n      return;', '   if (!radv_ps5_memory_alloc(RADV_PS5_RING_BYTES, RADV_PS5_LARGE_BYTES, true, &queue->ring)) {\n      fprintf(stderr, "radv/ps5: queue ring allocation failed\\n");\n      return;\n   }'),
        ('      radv_ps5_memory_free(&queue->ring);', '      fprintf(stderr, "radv/ps5: queue marker allocation failed\\n");\n      radv_ps5_memory_free(&queue->ring);'),
    ],
    'src/amd/vulkan/winsys/ps5/radv_ps5_platform.c': [
        ('''bool
radv_ps5_memory_grant_gpu(void *address, uint64_t bytes)
{
   /* A title's anonymous memory takes GPU access this way, and a shader then
    * writes it through the CPU's address (HARDWARE_FINDINGS.md, 2026-09-27). */
   return sceKernelMprotect(address, bytes,
                            PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_GPU_READ |
                               PS5_KERNEL_PROT_GPU_WRITE) == 0;
}''', (root/'tools/radv-host-memory.inc').read_text().strip()),
        ('   if (!placed) {', '   if (!placed) {\n      fprintf(stderr, "radv/ps5: mapping rejected result=%d address=%p bytes=%llu window32=%d\\n", result, address, (unsigned long long)bytes, window32);'),
    ] + RADV_SCATTER,
}.items():
    original = root.parent/'mihawk-mesa-review'/relative
    destination = root.parent/'mihawk-vulkan-review/.deps/work/radv-src'/relative
    adapted = original.read_text()
    for old, new in replacements:
        assert adapted.count(old) == 1, f'Pinned initialization anchor changed: {relative}'
        adapted = adapted.replace(old, new)
    if checking:
        check_ok = check_ok and destination.is_file() and destination.read_text() == adapted
        continue
    if destination.read_text() != adapted:
        destination.write_text(adapted)
        provenance = root.parent/'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
        if provenance.exists():
            provenance.replace(provenance.with_suffix('.base.txt'))
    print(f'RADV initialization diagnostic {relative} sha256={hashlib.sha256(adapted.encode()).hexdigest()}')
if checking:
    sys.exit(0 if check_ok else 1)
