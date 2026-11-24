// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <bit>
#include <utility>
#include "dynarmic/backend/x64/xbyak.h"

#include "dynarmic/backend/x64/a32_emit_x64.h"
#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/exclusive_monitor_friend.h"
#include "dynarmic/common/spin_lock_x64.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/ir/acc_type.h"

namespace Dynarmic::Backend::X64 {

namespace {

using namespace Xbyak::util;

constexpr size_t page_table_const_bits = 12;
constexpr size_t page_table_const_size = 1 << page_table_const_bits;
constexpr size_t page_table_const_mask = (1 << page_table_const_bits) - 1;

/// Where cold deferred code goes: the far region when there is one.
template<typename Ctx>
std::vector<std::function<void()>>& ColdEmits(BlockOfCode& code, Ctx& ctx) {
    return code.HasFarCode() ? ctx.far_deferred_emits : ctx.deferred_emits;
}

template<typename EmitContext>
void EmitDetectMisalignedVAddr(BlockOfCode& code, EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr, Xbyak::Reg64 tmp) {
    if (bitsize == 8 || (ctx.conf.detect_misaligned_access_via_page_table & bitsize) == 0) {
        return;
    }

    const u32 align_mask = [bitsize]() -> u32 {
        switch (bitsize) {
        case 16:
            return 0b1;
        case 32:
            return 0b11;
        case 64:
            return 0b111;
        case 128:
            return 0b1111;
        default:
            UNREACHABLE();
        }
    }();

    code.test(vaddr, align_mask);

    if (ctx.conf.only_detect_misalignment_via_page_table_on_page_boundary) {
        const u32 page_align_mask = static_cast<u32>(page_table_const_size - 1) & ~align_mask;

        SharedLabel detect_boundary = ctx.GenSharedLabel(), resume = ctx.GenSharedLabel();

        code.jnz(*detect_boundary, code.T_NEAR);
        code.L(*resume);

        ColdEmits(code, ctx).emplace_back([=, &code] {
            code.L(*detect_boundary);
            code.mov(tmp, vaddr);
            code.and_(tmp, page_align_mask);
            code.cmp(tmp, page_align_mask);
            code.jne(*resume, code.T_NEAR);
            // NOTE: We expect to fallthrough into abort code here.
        });
    } else {
        code.jnz(abort, code.T_NEAR);
    }
}

template<typename EmitContext>
Xbyak::RegExp EmitVAddrLookup(BlockOfCode& code, EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr, std::optional<std::pair<Xbyak::Reg64, Xbyak::Reg64>> scratch = std::nullopt);

template<>
[[maybe_unused]] Xbyak::RegExp EmitVAddrLookup<A32EmitContext>(BlockOfCode& code, A32EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr, std::optional<std::pair<Xbyak::Reg64, Xbyak::Reg64>> scratch) {
    const Xbyak::Reg64 page = scratch ? scratch->first : ctx.reg_alloc.ScratchGpr(code);
    const Xbyak::Reg64 tmp = scratch ? scratch->second : ctx.conf.absolute_offset_page_table && (ctx.conf.page_table_pointer_mask == 0 || (ctx.conf.page_table_pointer_mask == 0x00fffffffffff000ULL && ctx.conf.page_table_sign_extension == 8)) ? page : ctx.reg_alloc.ScratchGpr(code);

    EmitDetectMisalignedVAddr(code, ctx, bitsize, abort, vaddr, tmp);

    code.mov(tmp, vaddr);
    code.shr(tmp, int(page_table_const_bits));

    if (ctx.conf.page_table_log2_stride > 3) {
        code.shl(tmp, int(ctx.conf.page_table_log2_stride));
        code.mov(page, qword[r14 + tmp.cvt64()]);
    } else {
        code.mov(page, qword[r14 + tmp.cvt64() * int(1 << ctx.conf.page_table_log2_stride)]);
    }

    // check for marked bit, use as unmapped if marked
	if (ctx.conf.page_table_marked_bit) {
        if (*ctx.conf.page_table_marked_bit == 0) {
            code.test(page.cvt32(), 1);
            code.jnz(abort, code.T_NEAR);
        } else {
            code.bt(page, *ctx.conf.page_table_marked_bit);
            code.jc(abort, code.T_NEAR);
        }
	}
    // Eden packs pointer bits 12..55. Sign extension removes the upper
    // attributes first, leaving a page mask that fits an x86 immediate.
    if (ctx.conf.page_table_pointer_mask == 0x00fffffffffff000ULL &&
        ctx.conf.page_table_sign_extension == 8) {
        code.shl(page, 8);
        code.sar(page, 8);
        code.and_(page, -4096);
    } else {
    // mask away attributes
    if (ctx.conf.page_table_pointer_mask == 0) {
        code.test(page, page);
    } else if (std::in_range<s32>(ctx.conf.page_table_pointer_mask)) {
        code.and_(page, ctx.conf.page_table_pointer_mask);
    } else {
        code.mov(tmp, ctx.conf.page_table_pointer_mask);
        code.and_(page, tmp);
    }
    // check for sign bit, apply sign extension as needed
    if (ctx.conf.page_table_sign_extension) {
        code.shl(page, 63 - int(*ctx.conf.page_table_sign_extension));
        code.sar(page, 63 - int(*ctx.conf.page_table_sign_extension));
    }
    }

    code.jz(abort, code.T_NEAR);
    if (ctx.conf.absolute_offset_page_table) {
        return page + vaddr;
    }
    code.mov(tmp, vaddr);
    code.and_(tmp, u32(page_table_const_mask));
    return page + tmp.cvt64();
}

template<>
[[maybe_unused]] Xbyak::RegExp EmitVAddrLookup<A64EmitContext>(BlockOfCode& code, A64EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr, std::optional<std::pair<Xbyak::Reg64, Xbyak::Reg64>> scratch) {
    const size_t valid_page_index_bits = ctx.conf.page_table_address_space_bits - page_table_const_bits;
    const size_t unused_top_bits = 64 - ctx.conf.page_table_address_space_bits;

    const Xbyak::Reg64 page = scratch ? scratch->first : ctx.reg_alloc.ScratchGpr(code);
    const Xbyak::Reg64 tmp = scratch ? scratch->second : ctx.conf.absolute_offset_page_table && (ctx.conf.page_table_pointer_mask == 0 || (ctx.conf.page_table_pointer_mask == 0x00fffffffffff000ULL && ctx.conf.page_table_sign_extension == 8)) ? page : ctx.reg_alloc.ScratchGpr(code);

    EmitDetectMisalignedVAddr(code, ctx, bitsize, abort, vaddr, tmp);

    if (unused_top_bits == 0) {
        code.mov(tmp, vaddr);
        code.shr(tmp, int(page_table_const_bits));
    } else if (ctx.conf.silently_mirror_page_table) {
        if (valid_page_index_bits >= 32) {
            if (code.HasHostFeature(HostFeature::BMI2)) {
                const Xbyak::Reg64 bit_count = ctx.reg_alloc.ScratchGpr(code);
                code.mov(bit_count, unused_top_bits);
                code.bzhi(tmp, vaddr, bit_count);
                code.shr(tmp, int(page_table_const_bits));
                ctx.reg_alloc.Release(bit_count);
            } else {
                code.mov(tmp, vaddr);
                code.shl(tmp, int(unused_top_bits));
                code.shr(tmp, int(unused_top_bits + page_table_const_bits));
            }
        } else {
            code.mov(tmp, vaddr);
            code.shr(tmp, int(page_table_const_bits));
            code.and_(tmp, u32((1 << valid_page_index_bits) - 1));
        }
    } else {
        ASSERT(valid_page_index_bits < 32);
        code.mov(tmp, vaddr);
        code.shr(tmp, int(page_table_const_bits));
        code.test(tmp, u32(-(1 << valid_page_index_bits)));
        code.jnz(abort, code.T_NEAR);
    }

    if (ctx.conf.page_table_log2_stride > 3) {
        code.shl(tmp, int(ctx.conf.page_table_log2_stride));
        code.mov(page, qword[r14 + tmp.cvt64()]);
    } else {
        code.mov(page, qword[r14 + tmp.cvt64() * int(1 << ctx.conf.page_table_log2_stride)]);
    }

    // check for marked bit, use as unmapped if marked
    if (ctx.conf.page_table_marked_bit) {
        if (*ctx.conf.page_table_marked_bit == 0) {
            code.test(page.cvt32(), 1);
            code.jnz(abort, code.T_NEAR);
        } else {
            code.bt(page, *ctx.conf.page_table_marked_bit);
            code.jc(abort, code.T_NEAR);
        }
    }
    // Eden packs pointer bits 12..55. Sign extension removes the upper
    // attributes first, leaving a page mask that fits an x86 immediate.
    if (ctx.conf.page_table_pointer_mask == 0x00fffffffffff000ULL &&
        ctx.conf.page_table_sign_extension == 8) {
        code.shl(page, 8);
        code.sar(page, 8);
        code.and_(page, -4096);
    } else {
    // mask away attributes
    if (ctx.conf.page_table_pointer_mask == 0) {
        code.test(page, page);
    } else if (std::in_range<s32>(ctx.conf.page_table_pointer_mask)) {
        code.and_(page, ctx.conf.page_table_pointer_mask);
    } else {
        code.mov(tmp, ctx.conf.page_table_pointer_mask);
        code.and_(page, tmp);
    }
    // check for sign bit, apply sign extension as needed
    if (ctx.conf.page_table_sign_extension) {
        code.shl(page, *ctx.conf.page_table_sign_extension);
        code.sar(page, *ctx.conf.page_table_sign_extension);
    }
    }

    code.jz(abort, code.T_NEAR);
    if (ctx.conf.absolute_offset_page_table) {
        return page + vaddr;
    }
    code.mov(tmp, vaddr);
    code.and_(tmp, static_cast<u32>(page_table_const_mask));
    return page + tmp;
#if !defined(EDEN_A32_FASTMEM) // PS5 without the A32 fastmem window
#else // PS5
}

// Checked A32 fastmem: see headless/checked-fastmem.cmake.
constexpr int checked_map_bytes = 1 << 20;
constexpr u8 checked_read_blocked = 1, checked_write_blocked = 2;

inline void EmitCheckedFastmemTest(BlockOfCode& code, Xbyak::Reg64 vaddr, Xbyak::Reg64 index, u8 blocked_bit, Xbyak::Label& blocked) {
    code.mov(index.cvt32(), vaddr.cvt32());
    code.shr(index.cvt32(), int(page_table_const_bits));
    code.test(code.byte[r13 + index - checked_map_bytes], blocked_bit);
    code.jnz(blocked, code.T_NEAR);
}

// EmitVAddrLookup<A32EmitContext> for code inside a deferred block (instantiated for A32
// only): the misalignment check is inline because deferred emits cannot register more.
template<typename EmitContext>
Xbyak::RegExp EmitCheckedFallbackLookup(BlockOfCode& code, EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr, Xbyak::Reg64 page) {
    ASSERT(ctx.conf.absolute_offset_page_table);
    if (bitsize != 8 && (ctx.conf.detect_misaligned_access_via_page_table & bitsize) != 0) {
        const u32 align_mask = u32(bitsize / 8 - 1);
        Xbyak::Label aligned;
        code.test(vaddr, align_mask);
        code.jz(aligned, code.T_NEAR);
        if (ctx.conf.only_detect_misalignment_via_page_table_on_page_boundary) {
            const u32 page_align_mask = u32(page_table_const_size - 1) & ~align_mask;
            code.mov(page, vaddr);
            code.and_(page, page_align_mask);
            code.cmp(page, page_align_mask);
            code.je(abort, code.T_NEAR);
        } else {
            code.jmp(abort, code.T_NEAR);
        }
        code.L(aligned);
    }
    code.mov(page, vaddr);
    code.shr(page, int(page_table_const_bits));
    if (ctx.conf.page_table_log2_stride <= 3) {
        code.mov(page, qword[r14 + page * (1 << ctx.conf.page_table_log2_stride)]);
    } else {
        code.shl(page, int(ctx.conf.page_table_log2_stride));
        code.mov(page, qword[r14 + page]);
    }
    if (ctx.conf.page_table_marked_bit) {
        if (*ctx.conf.page_table_marked_bit == 0) {
            code.test(page.cvt32(), 1);
            code.jnz(abort, code.T_NEAR);
        } else {
            code.bt(page, *ctx.conf.page_table_marked_bit);
            code.jc(abort, code.T_NEAR);
        }
    }
    if (ctx.conf.page_table_pointer_mask == 0x00fffffffffff000ULL && ctx.conf.page_table_sign_extension == 8) {
        code.shl(page, 8);
        code.sar(page, 8);
        code.and_(page, -4096);
    } else {
        ASSERT(ctx.conf.page_table_pointer_mask == 0 || std::in_range<s32>(ctx.conf.page_table_pointer_mask));
        if (ctx.conf.page_table_pointer_mask == 0) {
            code.test(page, page);
        } else {
            code.and_(page, ctx.conf.page_table_pointer_mask);
        }
        if (ctx.conf.page_table_sign_extension) {
            code.shl(page, *ctx.conf.page_table_sign_extension);
            code.sar(page, *ctx.conf.page_table_sign_extension);
        }
    }
    code.jz(abort, code.T_NEAR);
    return page + vaddr;
#endif // PS5
}

template<typename EmitContext>
Xbyak::RegExp EmitExclusiveVAddrLookup(BlockOfCode& code, EmitContext& ctx, size_t bitsize,
        Xbyak::Label& abort, Xbyak::Reg64 vaddr, Xbyak::Reg64 page, Xbyak::Reg64 tmp) {
    if (bitsize > 8) {
        code.test(vaddr, u32(bitsize / 8 - 1));
        code.jnz(abort, code.T_NEAR);
    }
    // Natural alignment excludes page crossings, including CMPXCHG16B's
    // stricter alignment requirement. Skip the duplicate generic check.
    return EmitVAddrLookup(code, ctx, 8, abort, vaddr, std::make_optional(std::pair{page, tmp}));
}

template<typename EmitContext>
Xbyak::RegExp EmitFastmemVAddr(BlockOfCode& code, EmitContext& ctx, Xbyak::Label& abort, Xbyak::Reg64 vaddr, bool& require_abort_handling, std::optional<Xbyak::Reg64> tmp = std::nullopt);

template<>
[[maybe_unused]] Xbyak::RegExp EmitFastmemVAddr<A32EmitContext>(BlockOfCode&, A32EmitContext&, Xbyak::Label&, Xbyak::Reg64 vaddr, bool&, std::optional<Xbyak::Reg64>) {
    return r13 + vaddr;
}

template<>
[[maybe_unused]] Xbyak::RegExp EmitFastmemVAddr<A64EmitContext>(BlockOfCode& code, A64EmitContext& ctx, Xbyak::Label& abort, Xbyak::Reg64 vaddr, bool& require_abort_handling, std::optional<Xbyak::Reg64> tmp) {
    auto const unused_top_bits = 64 - ctx.conf.fastmem_address_space_bits;
    if (unused_top_bits == 0) {
        return r13 + vaddr;
    } else if (ctx.conf.silently_mirror_fastmem) {
        if (!tmp) {
            tmp = ctx.reg_alloc.ScratchGpr(code);
        }
        if (unused_top_bits < 32) {
            code.mov(*tmp, vaddr);
            code.shl(*tmp, int(unused_top_bits));
            code.shr(*tmp, int(unused_top_bits));
        } else if (unused_top_bits == 32) {
            code.mov(tmp->cvt32(), vaddr.cvt32());
        } else {
            code.mov(tmp->cvt32(), vaddr.cvt32());
            code.and_(*tmp, u32((1 << ctx.conf.fastmem_address_space_bits) - 1));
        }
        return r13 + *tmp;
    } else {
        if (ctx.conf.fastmem_address_space_bits < 32) {
            code.test(vaddr, u32(-(1 << ctx.conf.fastmem_address_space_bits)));
            code.jnz(abort, code.T_NEAR);
            require_abort_handling = true;
        } else {
            // TODO: Consider having TEST as above but coalesce 64-bit constant in register allocator
            if (!tmp) {
                tmp = ctx.reg_alloc.ScratchGpr(code);
            }
            code.mov(*tmp, vaddr);
            code.shr(*tmp, int(ctx.conf.fastmem_address_space_bits));
            code.jnz(abort, code.T_NEAR);
            require_abort_handling = true;
        }
        return r13 + vaddr;
    }
}

template<std::size_t bitsize>
const void* EmitReadMemoryMov(BlockOfCode& code, int value_idx, const Xbyak::RegExp& addr, bool ordered) {
    // An x86 load is already ordered before later loads and stores (acquire), and ordered stores
    // are xchg, which drains the store buffer: an ordered or exclusive load of up to 64 bits is a
    // plain mov (from Lichii #161). A locked xadd took the cache line exclusively on every core
    // that read it, and faulted on pages that are not writable.
    if (ordered && bitsize == 128) {
        code.xor_(eax, eax);
        code.xor_(ebx, ebx);
        code.xor_(ecx, ecx);
        code.xor_(edx, edx);

        const void* fastmem_location = code.getCurr();
        code.lock();
        code.cmpxchg16b(xword[addr]);
        if (code.HasHostFeature(HostFeature::SSE41)) {
            code.movq(Xbyak::Xmm(value_idx), rax);
            code.pinsrq(Xbyak::Xmm(value_idx), rdx, 1);
        } else {
            code.movq(Xbyak::Xmm(value_idx), rax);
            code.movq(xmm0, rdx);
            code.punpcklqdq(Xbyak::Xmm(value_idx), xmm0);
        }
        return fastmem_location;
    }
    const void* fastmem_location = code.getCurr();
    switch (bitsize) {
    case 8:
        code.movzx(Xbyak::Reg64(value_idx).cvt32(), code.byte[addr]);
        break;
    case 16:
        code.movzx(Xbyak::Reg64(value_idx).cvt32(), word[addr]);
        break;
    case 32:
        code.mov(Xbyak::Reg64(value_idx).cvt32(), dword[addr]);
        break;
    case 64:
        code.mov(Xbyak::Reg64(value_idx), qword[addr]);
        break;
    case 128:
        code.movups(Xbyak::Xmm(value_idx), xword[addr]);
        break;
    default:
        UNREACHABLE();
    }
    return fastmem_location;
}

template<std::size_t bitsize>
const void* EmitWriteMemoryMov(BlockOfCode& code, const Xbyak::RegExp& addr, int value_idx, bool ordered) {
    if (ordered) {
        if constexpr (bitsize == 128) {
            code.xor_(eax, eax);
            code.xor_(edx, edx);
            if (code.HasHostFeature(HostFeature::SSE41)) {
                code.movq(rbx, Xbyak::Xmm(value_idx));
                code.pextrq(rcx, Xbyak::Xmm(value_idx), 1);
            } else {
                code.movaps(xmm0, Xbyak::Xmm(value_idx));
                code.movq(rbx, xmm0);
                code.punpckhqdq(xmm0, xmm0);
                code.movq(rcx, xmm0);
            }
        }

        const void* fastmem_location = code.getCurr();
        switch (bitsize) {
        case 8:
            code.xchg(code.byte[addr], Xbyak::Reg64(value_idx).cvt8());
            break;
        case 16:
            code.xchg(word[addr], Xbyak::Reg64(value_idx).cvt16());
            break;
        case 32:
            code.xchg(dword[addr], Xbyak::Reg64(value_idx).cvt32());
            break;
        case 64:
            code.xchg(qword[addr], Xbyak::Reg64(value_idx));
            break;
        case 128: {
            Xbyak::Label loop;
            code.L(loop);
            code.lock();
            code.cmpxchg16b(xword[addr]);
            code.jnz(loop, code.T_NEAR);
            break;
        }
        default:
            UNREACHABLE();
        }
        return fastmem_location;
    } else {
        const void* fastmem_location = code.getCurr();
        switch (bitsize) {
        case 8:
            code.mov(code.byte[addr], Xbyak::Reg64(value_idx).cvt8());
            break;
        case 16:
            code.mov(word[addr], Xbyak::Reg64(value_idx).cvt16());
            break;
        case 32:
            code.mov(dword[addr], Xbyak::Reg64(value_idx).cvt32());
            break;
        case 64:
            code.mov(qword[addr], Xbyak::Reg64(value_idx));
            break;
        case 128:
            code.movups(xword[addr], Xbyak::Xmm(value_idx));
            break;
        default:
            UNREACHABLE();
        }
        return fastmem_location;
    }
}

template<typename UserConfig>
void EmitExclusiveLock(BlockOfCode& code, const UserConfig& conf, Xbyak::Reg64 pointer, Xbyak::Reg32 tmp) {
    if (conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return;
    }

    code.mov(pointer, std::bit_cast<u64>(GetExclusiveMonitorLockPointer(conf.global_monitor)));
    EmitSpinLockLock(code, pointer, tmp, code.HasHostFeature(HostFeature::WAITPKG));
}

template<typename UserConfig>
void EmitExclusiveUnlock(BlockOfCode& code, const UserConfig& conf, Xbyak::Reg64 pointer, Xbyak::Reg32 tmp) {
    if (conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return;
    }

    code.mov(pointer, std::bit_cast<u64>(GetExclusiveMonitorLockPointer(conf.global_monitor)));
    EmitSpinLockUnlock(code, pointer, tmp);
}

template<typename UserConfig>
void EmitExclusiveTestAndClear(BlockOfCode& code, const UserConfig& conf, Xbyak::Reg64 vaddr, Xbyak::Reg64 pointer, Xbyak::Reg64 tmp) {
    if (conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return;
    }

    code.mov(tmp, 0xDEAD'DEAD'DEAD'DEAD);
    const size_t processor_count = GetExclusiveMonitorProcessorCount(conf.global_monitor);
    for (size_t processor_index = 0; processor_index < processor_count; processor_index++) {
        if constexpr (!requires(const UserConfig& c) { c.tpidr_el0; }) {
            if (processor_index == conf.processor_id) {
                continue;
            }
        }
        Xbyak::Label ok;
        code.mov(pointer, std::bit_cast<u64>(GetExclusiveMonitorAddressPointer(conf.global_monitor, processor_index)));
        code.cmp(qword[pointer], vaddr);
        code.jne(ok, code.T_NEAR);
        code.mov(qword[pointer], tmp);
        code.L(ok);
    }
}

/// A folded address displacement as an Xbyak displacement (sign-extended).
inline size_t FoldedDisp(s32 disp) {
    return static_cast<size_t>(static_cast<s64>(disp));
}

inline bool IsOrdered(IR::AccType acctype) {
    return acctype == IR::AccType::ORDERED || acctype == IR::AccType::ORDEREDRW || acctype == IR::AccType::LIMITEDORDERED;
}

}  // namespace

}  // namespace Dynarmic::Backend::X64
