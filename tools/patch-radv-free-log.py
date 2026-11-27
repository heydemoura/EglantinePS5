#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""RADV_PS5_FREE_LOG=1: every buffer object's memory given back is written to stderr.

A GPU page fault reports only the address it wrote (GPU and CPU addresses are the same on the
console). With this log the address can be matched to the memory that was there and to when it
was given back. Off unless the variable is set. Applied after patch-radv-retire.py. Patches the
build export, never the Mesa checkout.
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
MARK = 'EglantinePS5 free log'
relative = 'src/amd/vulkan/winsys/ps5/radv_ps5_platform.c'
old = '''   radv_ps5_trim_untrack(memory);
   const int32_t unmapped = sceKernelMunmap(memory->cpu, memory->bytes);
'''
new = '''   radv_ps5_trim_untrack(memory);
   /* ''' + MARK + ''' (tools/patch-radv-free-log.py) */
   {
      static int wanted = -1;
      if (wanted < 0)
         wanted = getenv("RADV_PS5_FREE_LOG") != NULL;
      if (wanted)
         fprintf(stderr, "radv/ps5 free %p %" PRIx64 " at %u ms\\n", (void *)memory->cpu, memory->bytes,
                 radv_ps5_trim_now_ms());
   }
   const int32_t unmapped = sceKernelMunmap(memory->cpu, memory->bytes);
'''
path = source / relative
text = path.read_text()
if MARK not in text:
    if text.count(old) != 1:
        raise SystemExit(f'{relative}: pinned anchor changed ({text.count(old)}x)')
    path.write_text(text.replace(old, new))
print('RADV free log: RADV_PS5_FREE_LOG=1 writes each freed range to stderr')
