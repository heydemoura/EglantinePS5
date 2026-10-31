#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Names the places in a stderr.log that hold the console's shared memory.

  python3 -I tools/symbolize-sites.py STDERR_LOG build/headless-native/llvm-pie.elf

stderr.log has two kinds of lines, each with the return addresses found on the stack of the code that
asked for the memory (offsets from the image's start, as in a crash report):

  EDEN_HEAP_SITE <MiB> MiB in <n> blocks: <offsets>   a place that holds heap memory alive
  EDEN_CRAM_WRITER <n>: <offsets>                      a place that wrote guest memory (<n> slots)

The app's code is execute-only on the console, so the app cannot check that an address follows a call.
That is done here, with the image: addresses whose previous instruction is not a call are stale words of
the stack and are left out, and the rest are named, nearest call first.
"""
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

BASE = 0x400000


def tool(*names):
    for name in names:
        if shutil.which(name):
            return name
    sys.exit('needs one of: ' + ', '.join(names))


class Image:
    """The bytes of the image, looked up by virtual address."""

    def __init__(self, path):
        self.data = Path(path).read_bytes()
        d = self.data
        phoff = struct.unpack_from('<Q', d, 0x20)[0]
        phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
        self.loads = []
        for i in range(phnum):
            p_type, _flags, offset, vaddr, _paddr, filesz, _memsz, _align = struct.unpack_from(
                '<IIQQQQQQ', d, phoff + i * phentsize)
            if p_type == 1:
                self.loads.append((vaddr, offset, filesz))

    def bytes_before(self, address, count):
        for vaddr, offset, size in self.loads:
            if vaddr + count <= address <= vaddr + size:
                start = offset + (address - vaddr)
                return self.data[start - count:start]
        return b''


def follows_call(image, address):
    """Whether the instruction that ends at `address` is a call (direct, or through a register or memory)."""
    b = image.bytes_before(address, 7)
    if len(b) < 7:
        return False
    if b[-5] == 0xE8:                                        # call rel32
        return True
    if b[-2] == 0xFF and (b[-1] & 0xF8) in (0xD0, 0x10):     # call reg / call [reg]
        return True
    if b[-3] == 0xFF and (b[-2] & 0xF8) == 0x50:             # call [reg + disp8]
        return True
    if b[-4] == 0xFF and b[-3] == 0x54:                      # call [reg + index*s + disp8]
        return True
    if b[-6] == 0xFF and ((b[-5] & 0xF8) == 0x90 or b[-5] == 0x15):  # call [reg + disp32] / [rip + disp32]
        return True
    return False


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    log, elf = Path(sys.argv[1]), sys.argv[2]
    image = Image(elf)
    symbolizer = tool('llvm-symbolizer-18', 'llvm-symbolizer')
    lines = [line for line in log.read_text(errors='replace').splitlines()
             if line.startswith(('EDEN_HEAP_SITE ', 'EDEN_CRAM_WRITER '))]
    # The last report of each kind is the latest one: heap reports repeat, so keep the final block.
    heap = []
    for line in lines:
        if line.startswith('EDEN_HEAP_SITE '):
            heap.append(line)
    heap_block = []
    for line in log.read_text(errors='replace').splitlines():
        if line.startswith('EDEN_HEAP_SITES'):
            heap_block = []
        elif line.startswith('EDEN_HEAP_SITE '):
            heap_block.append(line)
    writers = []
    for line in log.read_text(errors='replace').splitlines():
        if line.startswith('EDEN_CRAM_WRITERS'):
            writers = []
        elif line.startswith('EDEN_CRAM_WRITER '):
            writers.append(line)

    cache = {}

    def raw(offset):
        out = subprocess.run([symbolizer, '-e', str(elf), '-f', '-C', '--no-inlines', hex(BASE + offset - 1)],
                             capture_output=True, text=True).stdout.splitlines()
        return (out[0] if out else '?')[:110]

    def name(offset):
        """The function of an address that follows a call; None for any other (a stale word)."""
        if offset not in cache:
            cache[offset] = raw(offset) if follows_call(image, BASE + offset) else None
        return cache[offset]

    def show(title, rows, pattern):
        print(title)
        for row in rows:
            m = re.match(pattern, row)
            if not m:
                continue
            amount, offsets = m.group(1), [int(x, 16) for x in m.group(2).split() if int(x, 16)]
            chain = [n for n in (name(o) for o in offsets) if n]
            if chain:
                text = ' <- '.join(chain[:3])
            else:
                # No return address among them: the nearest functions of the words, as a hint only.
                text = '? ' + ' | '.join(raw(o) for o in offsets[:2]) if offsets else '(no address)'
            print(f'  {amount:>10}  ' + text)

    show('Heap, largest places alive (MiB in blocks):', heap_block, r'EDEN_HEAP_SITE (\d+ MiB in \d+ blocks): (.*)')
    show('Guest memory, writers (slots promoted):', writers, r'EDEN_CRAM_WRITER (\d+): (.*)')


if __name__ == '__main__':
    main()
