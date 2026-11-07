# Shorter x64 sequences for three guest patterns that were hot in a large game's moving-route
# profile, and the A64 blocks' frame pointer set once per JIT entry. Backported from ProsperoEden
# (GPL-3.0-or-later). Opt-in: EDEN_JIT_FAST_PATHS=1 (env.txt) turns all four on, or one at a time
# EDEN_JIT_FCMP_ADC=1, EDEN_JIT_LIGHT_DMB=1, EDEN_JIT_CHECKBIT_FLAGS=1, EDEN_JIT_FRAME_ONCE=1. They are
# read once, when the first JIT is created (after the app applies env.txt), so the run-code prelude
# and every block agree; the log prints EDEN_JIT_FAST_PATHS with the result.
#
# - FCMP/FCMPE (FPCompare32/64): upstream turns ZF:CF into a shift count with `rcl cl, 5`, which
#   Zen 2 runs as a microcoded sequence. `adc cl, cl` (sete leaves the compare's CF) and `shl cl, 4`
#   give the same count, 16 * (2 * ZF + CF).
# - DMB: every variant was an mfence. On x86's ordering, loads are not reordered with later loads or
#   stores and stores are not reordered with later stores, so the A64 load-only and store-only
#   variants (CRm<1:0> = 01, 10) need no host fence; the remaining ones use a locked OR on the
#   block's own stack slot, which is a full fence for write-back memory and cheaper than mfence on
#   Zen 2. The JIT emits no non-temporal stores.
# - CBZ/CBNZ/TBZ/TBNZ (A64 CheckBit terminal): the condition was stored to the stack and loaded
#   back by the terminal. When the store is the block's last code before the frame-pointer restore
#   (which leaves EFLAGS alone), the setter also tests the register and the terminal branches on
#   those flags. The terminal checks the code emitted in between byte for byte and otherwise keeps
#   upstream's compare.
# - Frame pointer once: every block ran at the dispatcher's stack depth, so the per-block save, lea
#   and restore wrote the same values each time; RunCode/StepCode set it once instead.

function(eden_fast_paths_require source anchor what)
    string(FIND "${source}" "${anchor}" at)
    if(at LESS 0)
        message(FATAL_ERROR "Pinned JIT fast path changed (${what})")
    endif()
endfunction()

# A32/A64 emitter: called from the emitter loop in CMakeLists.txt before its write_derived.
macro(eden_fast_paths_emitter bits)
    set(eden_fp_dmb "void A${bits}EmitX64::EmitA${bits}DataMemoryBarrier(A${bits}EmitContext&, IR::Inst*) {
    code.mfence();
}
")
    eden_fast_paths_require("${emitter}" "${eden_fp_dmb}" "a${bits}_emit_x64.cpp DataMemoryBarrier")
    string(REPLACE "${eden_fp_dmb}" "void A${bits}EmitX64::EmitA${bits}DataMemoryBarrier(A${bits}EmitContext&, IR::Inst*) {
    // fast_paths.cmake: a locked RMW of the block's own spill slot is a full fence.
    if (__atomic_load_n(&eden_light_dmb, __ATOMIC_RELAXED)) {
        code.lock();
        code.or_(code.dword[rsp], 0);
    } else {
        code.mfence();
    }
}
" emitter "${emitter}")
    string(PREPEND emitter "extern \"C\" bool eden_light_dmb;  // fast_paths.cmake\n")
    if(${bits} EQUAL 64)
        set(eden_fp_prologue "    code.mov(code.qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, abi_base_pointer)], rbp);\n")
        set(eden_fp_set_check [==[void A64EmitX64::EmitA64SetCheckBit(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg8 to_store = ctx.reg_alloc.UseGpr(code, args[0]).cvt8();
    code.mov(code.byte[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, check_bit)], to_store);
}
]==])
        set(eden_fp_check_term [==[bool EmitTerminalImpl(A64EmitX64& e, IR::Term::CheckBit terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    Xbyak::Label fail;
    e.code.cmp(e.code.byte[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, check_bit)], u8(0));
    e.code.jz(fail);
]==])
        foreach(anchor IN ITEMS eden_fp_prologue eden_fp_set_check eden_fp_check_term)
            eden_fast_paths_require("${emitter}" "${${anchor}}" "a64_emit_x64.cpp ${anchor}")
        endforeach()
        string(REPLACE "${eden_fp_prologue}" "    eden_checkbit_mark = nullptr;\n${eden_fp_prologue}" emitter "${emitter}")
        # Frame pointer once per entry (jit_frame_once): every block runs at the dispatcher's stack
        # depth, so the per-block save, lea and restore wrote the same values each time; RunCode and
        # StepCode set it once (CMakeLists.txt, block_of_code.cpp) and the callee-save pop restores it.
        set(eden_fp_frame "${eden_fp_prologue}    code.lea(rbp, code.ptr[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, abi_base_pointer) - 8]);\n")
        set(eden_fp_restore "    code.mov(rbp, code.qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, abi_base_pointer)]);\n    EmitTerminal(")
        eden_fast_paths_require("${emitter}" "${eden_fp_frame}" "a64_emit_x64.cpp frame pointer")
        eden_fast_paths_require("${emitter}" "${eden_fp_restore}" "a64_emit_x64.cpp frame pointer restore")
        string(REPLACE "${eden_fp_frame}" "    if (!eden_jit_frame_once) {\n    ${eden_fp_prologue}    code.lea(rbp, code.ptr[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, abi_base_pointer) - 8]);\n    }\n" emitter "${emitter}")
        string(REPLACE "${eden_fp_restore}" "    if (!eden_jit_frame_once) code.mov(rbp, code.qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, abi_base_pointer)]);\n    EmitTerminal(" emitter "${emitter}")
        string(REPLACE "${eden_fp_set_check}" [==[void A64EmitX64::EmitA64SetCheckBit(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg8 to_store = ctx.reg_alloc.UseGpr(code, args[0]).cvt8();
    code.mov(code.byte[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, check_bit)], to_store);
    eden_checkbit_mark = nullptr;
    if (__atomic_load_n(&eden_checkbit_flags, __ATOMIC_RELAXED)) {
        // fast_paths.cmake: ZF for the CheckBit terminal, if nothing else follows.
        code.test(to_store, to_store);
        eden_checkbit_mark = code.getCurr();
    }
}
]==] emitter "${emitter}")
        string(REPLACE "${eden_fp_check_term}" [==[bool EmitTerminalImpl(A64EmitX64& e, IR::Term::CheckBit terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    Xbyak::Label fail;
    // fast_paths.cmake: the setter's test still holds when the only code since is
    // Emit's frame-pointer restore, mov rbp, [rsp + 0x418].
    static constexpr unsigned char restore_rbp[] = {0x48, 0x8B, 0xAC, 0x24, 0x18, 0x04, 0x00, 0x00};
    constexpr bool restore_layout = ABI_SHADOW_SPACE + offsetof(StackLayout, abi_base_pointer) == 0x418;
    const unsigned char* const mark = eden_checkbit_mark;
    eden_checkbit_mark = nullptr;
    // With the frame pointer set once per entry (jit_frame_once) nothing follows the setter at all.
    if ((mark && eden_jit_frame_once && e.code.getCurr() == mark) ||
        (restore_layout && mark && e.code.getCurr() == mark + sizeof(restore_rbp) &&
         std::memcmp(mark, restore_rbp, sizeof(restore_rbp)) == 0)) {
        __atomic_fetch_add(&eden_checkbit_fused, 1, __ATOMIC_RELAXED);
    } else {
        e.code.cmp(e.code.byte[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, check_bit)], u8(0));
    }
    e.code.jz(fail);
]==] emitter "${emitter}")
        string(PREPEND emitter "#include <cstring>
extern \"C\" bool eden_checkbit_flags;  // fast_paths.cmake
extern \"C\" bool eden_jit_frame_once;  // fast_paths.cmake, read before any JIT exists
extern \"C\" unsigned long long eden_checkbit_fused;
static thread_local const unsigned char* eden_checkbit_mark = nullptr;
")
    endif()
endmacro()

# FP compare and the A64 DMB translation: called with the other dynarmic overlays.
macro(eden_fast_paths_sources)
    set(eden_fp_dir "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic")
    file(READ "${eden_fp_dir}/backend/x64/emit_x64_floating_point.cpp" eden_fp_source)
    set(eden_fp_rcl "    code.sete(cl);\n    code.rcl(cl, 5);  // cl = ZF:CF:0000\n")
    eden_fast_paths_require("${eden_fp_source}" "${eden_fp_rcl}" "emit_x64_floating_point.cpp SetFpscrNzcvFromFlags")
    string(REPLACE "${eden_fp_rcl}" [==[    code.sete(cl);
    if (__atomic_load_n(&eden_fcmp_adc, __ATOMIC_RELAXED)) {
        // fast_paths.cmake: sete leaves the compare's CF; same count, no microcode.
        code.adc(cl, cl);  // cl = ZF:CF
        code.shl(cl, 4);   // cl = ZF:CF:0000
    } else {
        code.rcl(cl, 5);  // cl = ZF:CF:0000
    }
]==] eden_fp_source "${eden_fp_source}")
    string(PREPEND eden_fp_source [==[#include <cstdio>
#include <cstdlib>
// The fast-path switches (fast_paths.cmake) and their one-time configuration from env.txt.
extern "C" {
bool eden_fcmp_adc = false, eden_light_dmb = false, eden_checkbit_flags = false, eden_jit_frame_once = false;
unsigned long long eden_dmb_dropped = 0, eden_checkbit_fused = 0;
void eden_fast_paths_configure() {
    static bool configured = false;
    if (__atomic_exchange_n(&configured, true, __ATOMIC_ACQ_REL)) return;
    const auto on = [](const char* name) {
        const char* all = std::getenv("EDEN_JIT_FAST_PATHS");
        const char* one = std::getenv(name);
        return (all != nullptr && all[0] == '1') || (one != nullptr && one[0] == '1');
    };
    // Only ever turns a switch on: tests set them directly.
    if (on("EDEN_JIT_FCMP_ADC")) eden_fcmp_adc = true;
    if (on("EDEN_JIT_LIGHT_DMB")) eden_light_dmb = true;
    if (on("EDEN_JIT_CHECKBIT_FLAGS")) eden_checkbit_flags = true;
    if (on("EDEN_JIT_FRAME_ONCE")) eden_jit_frame_once = true;
    std::printf("EDEN_JIT_FAST_PATHS fcmp_adc=%d light_dmb=%d checkbit_flags=%d frame_once=%d\n", eden_fcmp_adc,
                eden_light_dmb, eden_checkbit_flags, eden_jit_frame_once);
}
}
]==])
    write_derived("${PORT_BUILD_DIR}/emit_x64_floating_point.cpp" "${eden_fp_source}")
    get_target_property(eden_fp_sources dynarmic SOURCES)
    list(FILTER eden_fp_sources EXCLUDE REGEX "(^|/)backend/x64/emit_x64_floating_point\\.cpp$")
    set_property(TARGET dynarmic PROPERTY SOURCES "${eden_fp_sources}")
    target_sources(dynarmic PRIVATE "${PORT_BUILD_DIR}/emit_x64_floating_point.cpp")

    # a64_translate.cpp includes this file; CMakeLists.txt points that include at the derived copy.
    file(READ "${eden_fp_dir}/frontend/A64/translate/impl/system.cpp" eden_fp_system)
    set(eden_fp_dmb_translate [==[bool TranslatorVisitor::DMB(Imm<4> /*CRm*/) {
    ir.DataMemoryBarrier();
    return true;
}
]==])
    eden_fast_paths_require("${eden_fp_system}" "${eden_fp_dmb_translate}" "system.cpp DMB")
    string(REPLACE "${eden_fp_dmb_translate}" [==[bool TranslatorVisitor::DMB(Imm<4> CRm) {
    // fast_paths.cmake: x86 already keeps the order DMB *LD and DMB *ST ask for.
    const u32 types = CRm.ZeroExtend() & 3;
    if ((types == 1 || types == 2) && __atomic_load_n(&eden_light_dmb, __ATOMIC_RELAXED)) {
        __atomic_fetch_add(&eden_dmb_dropped, 1, __ATOMIC_RELAXED);
        return true;
    }
    ir.DataMemoryBarrier();
    return true;
}
]==] eden_fp_system "${eden_fp_system}")
    string(PREPEND eden_fp_system "extern \"C\" bool eden_light_dmb;  // fast_paths.cmake\nextern \"C\" unsigned long long eden_dmb_dropped;\n")
    write_derived("${PORT_BUILD_DIR}/a64_system.cpp" "${eden_fp_system}")
    set(EDEN_FAST_PATHS_SYSTEM "${PORT_BUILD_DIR}/a64_system.cpp")
endmacro()
