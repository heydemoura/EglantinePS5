#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the PS5 executable's own memcmp (src/fast_memcmp.c) on this machine.

The console build cannot be run here, and a wrong memcmp would show as anything from a missed
cache entry to a corrupted texture. So the same source is built with the system compiler and
compared with the host's memcmp and with a plain byte loop:

- equal buffers of every length from 0 to 4096 at every pair of alignments (0-31 each);
- up to 96 bytes: a differing byte at every position, at every pair of alignments, in both
  directions, with byte values on both sides of 0x80 (the result's sign is that of unsigned
  bytes), and again with a later difference the other way (the first one decides);
- longer ones: differences at the ends, around the 32- and 8-byte steps and at random positions;
- every length with both buffers ending at an unreadable page: a load past the end faults.

Then the program is built once more with the source's PS5_NATIVE part, which names the function
memcmp and bcmp: it must be the memcmp that program calls, with none imported from the libc.
And, when the PS5 build's compiler is here (clang-18, tools/ps5.cmake), the object built for the
console must define memcmp and bcmp hidden and call nothing (a compiler that turned the loops into
a call of memcmp would make the function call itself).

CC and CXX name the compilers (cc and c++ by default). Needs a CPU with AVX2, as the console's.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = root / 'src/fast_memcmp.c'

PROGRAM = r'''
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include <sys/mman.h>
#include <unistd.h>

extern "C" int eden_fast_memcmp(const void*, const void*, size_t);

namespace {
// The host's memcmp, through a pointer the compiler cannot see through: a direct call with a
// known size would be done inline and never reach the library.
int (*volatile host_memcmp)(const void*, const void*, size_t) = std::memcmp;

int Sign(int value) {
    return value < 0 ? -1 : value > 0;
}

// What memcmp means. Volatile reads keep it a byte loop whatever the optimiser knows.
int Plain(const volatile unsigned char* a, const volatile unsigned char* b, size_t size) {
    for (size_t at = 0; at < size; ++at) {
        const unsigned char x = a[at], y = b[at];
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

unsigned long long checks, wrong;

void Check(const unsigned char* a, const unsigned char* b, size_t size, int expected, const char* what) {
    const int ours = Sign(eden_fast_memcmp(a, b, size));
    const int host = Sign(host_memcmp(a, b, size));
    ++checks;
    if (ours == expected && host == expected) return;
    if (wrong++ < 16)
        std::fprintf(stderr, "wrong: %s, %zu bytes, alignments %zu and %zu: ours %d, the host's %d, expected %d\n",
                     what, size, static_cast<size_t>(reinterpret_cast<uintptr_t>(a) % 32),
                     static_cast<size_t>(reinterpret_cast<uintptr_t>(b) % 32), ours, host, expected);
}

// Byte pairs that differ, lower first as unsigned bytes: as signed chars several order the other way.
constexpr unsigned char pairs[][2] = {{0x00, 0x01}, {0x00, 0xff}, {0x7f, 0x80}, {0x01, 0x80},
                                      {0xfe, 0xff}, {0x10, 0x50}, {0x00, 0x80}, {0x7f, 0xff}};
constexpr size_t pair_count = sizeof(pairs) / sizeof(pairs[0]);

// Two buffers with the same bytes get a difference at `position`, each way round, and then, when
// `later` is inside them, a second one after it the other way: the first still decides. The
// bytes are put back.
void CheckDifference(unsigned char* a, unsigned char* b, size_t size, size_t position, size_t later,
                     size_t pair) {
    const unsigned char kept = a[position];
    a[position] = pairs[pair][0];
    b[position] = pairs[pair][1];
    Check(a, b, size, Plain(a, b, size), "one difference");
    Check(b, a, size, Plain(b, a, size), "one difference, exchanged");
    if (later > position && later < size) {
        const unsigned char kept_later = a[later];
        a[later] = 0xff;
        b[later] = 0x00;
        Check(a, b, size, -1, "a later difference the other way");
        Check(b, a, size, 1, "a later difference the other way, exchanged");
        a[later] = b[later] = kept_later;
    }
    a[position] = b[position] = kept;
}

// `pages` readable pages followed by one that is not: a buffer placed to end at `end` cannot be
// read past its last byte without a fault.
unsigned char* GuardedEnd(size_t pages) {
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* const memory = mmap(nullptr, (pages + 1) * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED || mprotect(static_cast<unsigned char*>(memory) + pages * page, page, PROT_NONE) != 0) {
        std::perror("guard page");
        std::exit(2);
    }
    return static_cast<unsigned char*>(memory) + pages * page;
}
} // namespace

int main() {
    if (!__builtin_cpu_supports("avx2")) {
        std::fprintf(stderr, "this CPU has no AVX2\n");
        return 77;
    }
    const bool aliased = reinterpret_cast<void*>(host_memcmp) == reinterpret_cast<void*>(&eden_fast_memcmp);
#ifdef EXPECT_ALIASED
    if (!aliased) {
        std::fprintf(stderr, "memcmp is not the function from src/fast_memcmp.c in a program linked with it\n");
        return 1;
    }
#else
    if (aliased) {
        std::fprintf(stderr, "the host's memcmp was replaced: there is nothing to compare with\n");
        return 1;
    }
#endif

    constexpr size_t longest = 4096;
    constexpr size_t small = 96;
    constexpr size_t arena = longest + 64;
    alignas(32) static unsigned char left[arena], right[arena];
    std::mt19937_64 random{0x45676c616e74696eULL};
    for (size_t i = 0; i < arena; ++i) left[i] = right[i] = static_cast<unsigned char>(random());

    // Every pair of alignments (the arrays start on a 32-byte boundary).
    for (size_t a = 0; a < 32; ++a)
        for (size_t b = 0; b < 32; ++b) {
            std::memcpy(right + b, left + a, longest);
            // Equal buffers of every length.
            for (size_t size = 0; size <= longest; ++size)
                Check(left + a, right + b, size, 0, "equal buffers");
            // Small lengths: a differing byte at every position.
            for (size_t size = 1; size <= small; ++size)
                for (size_t position = 0; position < size; ++position)
                    CheckDifference(left + a, right + b, size, position,
                                    position + 1 + (size * 7 + position * 3 + a + b) % small,
                                    (size + position + a * 3 + b) % pair_count);
        }

    // Longer lengths: differences at the ends, around the 32- and 8-byte steps, and at random places.
    for (size_t size = small + 1; size <= longest; ++size) {
        for (int round = 0; round < 6; ++round) {
            const size_t a = round == 0 ? 0 : random() % 32, b = round == 0 ? 0 : random() % 32;
            std::memcpy(right + b, left + a, size);
            const size_t tail = size - size % 32, words = size - size % 8;
            const size_t places[] = {0, 1, 7, 8, 31, 32, 33, 63, 64, size / 2, tail - 1, tail, tail + 7,
                                     words - 1, words, size - 9, size - 8, size - 2, size - 1,
                                     static_cast<size_t>(random() % size), static_cast<size_t>(random() % size)};
            for (const size_t position : places) {
                // (Named, so that the two draws happen in this order with every compiler.)
                const size_t later = position + 1 + random() % size, pair = random() % pair_count;
                if (position < size) CheckDifference(left + a, right + b, size, position, later, pair);
            }
        }
    }

    // Both buffers ending at an unreadable page, every length: a load past the end is a fault.
    unsigned char* const end_a = GuardedEnd(2);
    unsigned char* const end_b = GuardedEnd(2);
    for (size_t i = 0; i < longest; ++i)
        (end_a - longest)[i] = (end_b - longest)[i] = static_cast<unsigned char>(random());
    for (size_t size = 0; size <= longest; ++size) {
        unsigned char* const a = end_a - size;
        unsigned char* const b = end_b - size;
        Check(a, b, size, 0, "equal buffers at the end of readable memory");
        // One at the end of memory, the other anywhere.
        std::memcpy(right + size % 32, a, size);
        Check(a, right + size % 32, size, 0, "one buffer at the end of readable memory");
        Check(right + size % 32, a, size, 0, "one buffer at the end of readable memory, exchanged");
        if (size == 0) continue;
        for (const size_t position : {size_t{0}, size / 2, size - 1, static_cast<size_t>(random() % size)}) {
            const size_t later = position + 1 + random() % size, pair = random() % pair_count;
            CheckDifference(a, b, size, position, later, pair);
        }
    }

    std::printf("%llu comparisons, %llu wrong%s\n", checks, wrong,
                aliased ? " (memcmp itself is the function checked)" : "");
    return wrong == 0 ? 0 : 1;
}
'''


def run(command, **options):
    return subprocess.run([str(part) for part in command], check=True, **options)


def tool(*names):
    return next((path for path in map(shutil.which, names) if path), None)


cc = os.environ.get('CC', 'cc')
cxx = os.environ.get('CXX', 'c++')
with tempfile.TemporaryDirectory(prefix='eden-memcmp-') as scratch:
    scratch = Path(scratch)
    program = scratch / 'check.cpp'
    program.write_text(PROGRAM)
    for name, defines, expect in (('host', [], []), ('aliased', ['-DPS5_NATIVE=1'], ['-DEXPECT_ALIASED'])):
        # The function at the optimisation the console build uses (Release: -O3).
        run([cc, '-O3', '-Wall', '-Wextra', '-Werror', *defines, '-c', source, '-o', scratch / f'{name}.o'])
        exe = scratch / name
        run([cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', *expect, program, scratch / f'{name}.o', '-o', exe])
        result = subprocess.run([str(exe)], timeout=600)
        if result.returncode == 77:
            raise SystemExit('memcmp check not run: this CPU has no AVX2 (the console has)')
        if result.returncode != 0:
            raise SystemExit(f'memcmp check failed ({name}): exit {result.returncode}')
        nm = tool('nm', 'llvm-nm', 'llvm-nm-18')
        if nm:
            imports = subprocess.run([nm, '-D', '--undefined-only', str(exe)], check=True, capture_output=True,
                                     text=True).stdout.split()
            imported = any(symbol.split('@')[0] in ('memcmp', 'bcmp') for symbol in imports)
            if name == 'aliased' and imported:
                raise SystemExit('a program linked with the PS5 part of src/fast_memcmp.c still imports memcmp')
            if name == 'host' and not imported:
                raise SystemExit('the comparison program does not import the host memcmp: nothing was compared')
    print('memcmp: agrees with the host memcmp and a byte loop; no read past the end; linked in, it is memcmp')

    # The object as the console build makes it (tools/ps5.cmake's flags), without and with
    # EDEN_CPU_PRESET=zen2. Only compiled: no SDK header is needed for it.
    clang, readelf, nm = tool('clang-18'), tool('llvm-readelf-18', 'llvm-readelf'), tool('llvm-nm-18', 'llvm-nm')
    if not (clang and readelf and nm):
        print('memcmp: clang-18 or the LLVM object tools are not here; the console object was not inspected')
        sys.exit(0)
    for preset in ([], ['-march=znver2', '-mtune=znver2']):
        target = scratch / 'ps5.o'
        run([clang, '--target=x86_64-sie-ps5', '-ffreestanding', '-O3', '-fPIC', '-fno-stack-protector', '-fno-plt',
             '-femulated-tls', '-fvisibility-nodllstorageclass=default', '-ffunction-sections', '-fdata-sections',
             *preset, '-DPS5_NATIVE=1', '-Wall', '-Wextra', '-Werror', '-c', source, '-o', target])
        undefined = subprocess.run([nm, '--undefined-only', str(target)], check=True, capture_output=True,
                                   text=True).stdout.split()
        if undefined:
            raise SystemExit(f'the console object calls out ({" ".join(undefined)}): memcmp must call nothing')
        symbols = subprocess.run([readelf, '-s', str(target)], check=True, capture_output=True, text=True).stdout
        for wanted in ('memcmp', 'bcmp'):
            rows = [row.split() for row in symbols.splitlines() if row.split()[-1:] == [wanted]]
            if len(rows) != 1 or rows[0][3:6] != ['FUNC', 'GLOBAL', 'HIDDEN']:
                raise SystemExit(f'the console object does not define {wanted} as a hidden function: {rows}')
    print('memcmp: the console object defines memcmp and bcmp hidden and calls nothing')
