// SPDX-License-Identifier: GPL-3.0-or-later
/* memcmp for the whole PS5 executable.
 *
 * Ported from PS5_Lichii, ps5/src/fast_mem.cpp at commit 5a424f7 (GPL-3.0-or-later; its
 * docs/ps5/gpu-thread.md has the measurements). The executable imports memcmp from the console's
 * libSceLibcInternal, which compares a byte at a time: Lichii measured 14 ns for 8 bytes and
 * 218 ns for 2 KiB there, against 2.9 and 38 ns for this one, and memcmp at 2.6% of its GPU back
 * thread in a race. The library's memcpy is as fast as one written here and is left alone.
 *
 * tools/check-memcmp.py compares this with the host's memcmp. */
#include <stddef.h>
#include <stdint.h>

#include <immintrin.h>

/* Eight bytes at any address, of any object's storage. */
typedef uint64_t __attribute__((may_alias, aligned(1))) eden_unaligned_u64;

/* The compiler must not turn the loops below into a call of memcmp or bcmp: under those names
 * this function is what such a call reaches. */
#if defined(__has_attribute) && __has_attribute(no_builtin)
#define EDEN_NO_BUILTIN __attribute__((no_builtin))
#else
#define EDEN_NO_BUILTIN
#endif

/* AVX2 for this function alone: the console's CPU always has it, while the executable is built
 * for generic x86-64 unless EDEN_CPU_PRESET=zen2 (headless/ps5.cmake). The compiler clears the
 * upper halves of the registers before each return, so SSE callers pay nothing for it. */
EDEN_NO_BUILTIN __attribute__((target("avx2")))
int eden_fast_memcmp(const void *left, const void *right, size_t size)
{
    const unsigned char *const a = (const unsigned char *)left;
    const unsigned char *const b = (const unsigned char *)right;
    size_t at = 0;
    /* No load reaches past `size`: callers compare up to the end of mapped memory. */
    for (; at + 32 <= size; at += 32) {
        const uint32_t differ = ~(uint32_t)_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(a + at)),
                              _mm256_loadu_si256((const __m256i *)(b + at))));
        if (differ != 0) {
            at += (size_t)__builtin_ctz(differ);
            return (int)a[at] - (int)b[at];
        }
    }
    for (; at + 8 <= size; at += 8) {
        const uint64_t x = *(const eden_unaligned_u64 *)(a + at);
        const uint64_t y = *(const eden_unaligned_u64 *)(b + at);
        if (x != y) {
            /* Little-endian: the lowest differing bit is in the first differing byte. */
            at += (size_t)__builtin_ctzll(x ^ y) >> 3;
            return (int)a[at] - (int)b[at];
        }
    }
    for (; at < size; ++at) {
        if (a[at] != b[at])
            return (int)a[at] - (int)b[at];
    }
    return 0;
}

#ifdef PS5_NATIVE
/* Under these names it takes the place of the system library's for every caller linked into the
 * executable, the Vulkan driver's included: a definition in the executable's own objects wins
 * over a shared library's, so no import of memcmp is left. Hidden, and by the assembler (Lichii:
 * the attribute on a definition of memcmp did not hide it), so that it never becomes an export of
 * the executable. bcmp has no caller today; it is named so that one compiled later does not go
 * back to the library. Only the PS5 build defines PS5_NATIVE for this file: host builds keep
 * their libc's memcmp. */
__asm__(".globl memcmp\n.hidden memcmp\n.set memcmp, eden_fast_memcmp\n"
        ".globl bcmp\n.hidden bcmp\n.set bcmp, eden_fast_memcmp\n");
#endif
