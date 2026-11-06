# Per-engine matchers must have the same layout in every translation unit.
file(READ "${PROJECT_SOURCE_DIR}/src/video_core/macro.h" macro_header)
set(macro_member "    u64 native_code_generation{};")
string(FIND "${macro_header}" "${macro_member}" macro_member_at)
if(macro_member_at LESS 0)
    message(FATAL_ERROR "Pinned MacroEngine code generation changed")
endif()
string(REPLACE "${macro_member}"
    "${macro_member}\n    ::Eden::MacroNative::Matcher native_front_matches, native_back_matches;"
    macro_header "${macro_header}")
write_derived("${PORT_BUILD_DIR}/macro-native/video_core/macro.h"
    "#include \"${EDEN_PORT_DIR}/macro_matcher.h\"\n${macro_header}")
# Derive from the existing PS5 recovery source when present, retaining its
# interpreter restriction and exception-safe JIT callbacks.
if(PS5_NATIVE AND EDEN_PS5_OPENGL)
    set(macro_input "${PORT_BUILD_DIR}/macro_recovery.cpp")
else()
    set(macro_input "${PROJECT_SOURCE_DIR}/src/video_core/macro.cpp")
endif()
file(READ "${macro_input}" native_macro_source)
set(native_macro_anchor "        // Reused between calls (a macro never calls another: methods >= 0xe00 are refused below);")
string(FIND "${native_macro_source}" "${native_macro_anchor}" native_macro_at)
if(native_macro_at LESS 0)
    message(FATAL_ERROR "Pinned MacroEngine native dispatch changed")
endif()
string(REPLACE "${native_macro_anchor}" [=[        ::Eden::MacroNative::Bench native_bench{native_back_matches, native_code, native_code_generation, method, "back"};
        if (::Eden::MacroNative::TryRun(native_back_matches, native_code, native_code_generation,
                method, parameters, std::span<u32>{maxwell3d.GetNativeDataRam()},
                [&](u32 address) { return address < Maxwell3D::Regs::NUM_REGS ? maxwell3d.GetRegisterValue(address) : 0U; },
                [&](u32 address, u32 value) {
                    ::Eden::MacroNative::EmitBench emit_bench{native_bench};
                    VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
                    maxwell3d.MacroWrite(system, address, value);
                }, "back")) return;
        // Reused between calls (a macro never calls another: methods >= 0xe00 are refused below);
]=] native_macro_source "${native_macro_source}")
set(native_emit "        const auto emit = [&](u32 address, u32 value) {")
string(FIND "${native_macro_source}" "${native_emit}" native_emit_at)
if(native_emit_at LESS 0)
    message(FATAL_ERROR "Pinned native macro method callback changed")
endif()
string(REPLACE "${native_emit}" "${native_emit}\n                ::Eden::MacroNative::EmitBench emit_bench{native_bench};"
    native_macro_source "${native_macro_source}")
write_derived("${PORT_BUILD_DIR}/macro_native.cpp"
    "#include \"${EDEN_PORT_DIR}/macro_native_hook.h\"\n${native_macro_source}")
list(REMOVE_ITEM video_sources macro.cpp "${PORT_BUILD_DIR}/macro_recovery.cpp")
list(APPEND video_sources "${PORT_BUILD_DIR}/macro_native.cpp")
if(EDEN_DEV_PROFILE)
    file(READ "${PORT_BUILD_DIR}/maxwell_3d.cpp" native_maxwell)
else()
    file(READ "${PROJECT_SOURCE_DIR}/src/video_core/engines/maxwell_3d.cpp" native_maxwell)
endif()
set(front_begin "        engine.native_interpreter.active_dram = &native_data_ram;")
set(front_end "    fs.parameters.clear();")
string(FIND "${native_maxwell}" "${front_begin}" front_begin_at)
string(FIND "${native_maxwell}" "${front_end}" front_end_at)
if(front_begin_at LESS 0 OR front_end_at LESS 0)
    message(FATAL_ERROR "Pinned Maxwell3D front macro dispatch changed")
endif()
string(REPLACE "${front_begin}" [=[        ::Eden::MacroNative::Bench native_bench{engine.native_front_matches, engine.native_code, engine.native_code_generation, start, "front"};
        if (!::Eden::MacroNative::TryRun(engine.native_front_matches, engine.native_code,
                engine.native_code_generation, start, fs.parameters, std::span<u32>{native_data_ram},
                [&fs](u32 address) { return address < Regs::NUM_REGS ? fs.regs[address] : 0U; },
                [&](u32 address, u32 value) { ::Eden::MacroNative::EmitBench emit_bench{native_bench}; FrontWrite(split, fs, address, value, true, true); }, "front")) {
        engine.native_interpreter.active_dram = &native_data_ram;
]=] native_maxwell "${native_maxwell}")
set(front_emit "            [&](u32 address, u32 value) {\n")
string(FIND "${native_maxwell}" "${front_emit}" front_emit_at)
if(front_emit_at LESS 0)
    message(FATAL_ERROR "Pinned front macro method callback changed")
endif()
string(REPLACE "${front_emit}" "${front_emit}                ::Eden::MacroNative::EmitBench emit_bench{native_bench};\n"
    native_maxwell "${native_maxwell}")
# Close just before the original scope's end, preserving its error handling.
set(front_close "    }\n    fs.parameters.clear();")
string(REPLACE "${front_close}" "        }\n${front_close}" native_maxwell "${native_maxwell}")
write_derived("${PORT_BUILD_DIR}/maxwell_3d.cpp"
    "#include \"${EDEN_PORT_DIR}/macro_native_hook.h\"\n${native_maxwell}")
list(REMOVE_ITEM video_sources engines/maxwell_3d.cpp "${PORT_BUILD_DIR}/maxwell_3d.cpp")
list(APPEND video_sources "${PORT_BUILD_DIR}/maxwell_3d.cpp")
target_link_libraries(video_core PUBLIC OpenSSL::Crypto)
