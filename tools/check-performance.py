#!/usr/bin/env python3
"""Keep diagnostic collection out of production guest, JIT and storage hot paths."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
generated = cache / 'native-local/headless'
config = (generated.parent / 'CMakeCache.txt').read_text()
development = 'EDEN_DEV_PROFILE:BOOL=ON' in config
diagnostics = 'EDEN_MKW_DIAGNOSTICS:BOOL=ON' in config

for name in ('a32_interface.cpp', 'a64_interface.cpp', 'block_of_code.cpp',
             'arm_dynarmic_32.cpp', 'arm_dynarmic_64.cpp'):
    source = (generated / name).read_text()
    diagnostic_wrapper = diagnostics and name in ('arm_dynarmic_32.cpp', 'arm_dynarmic_64.cpp')
    if not diagnostic_wrapper:
        assert 'Eden::Performance::Timer' not in source, name
    if not development:
        assert 'Eden::Performance::SampleCpu' not in source
    assert 'Eden::Performance::cpu_state' not in source

jit = (generated / 'a64_interface.cpp').read_text()
block = jit.split('    CodePtr GetBlock(', 1)[1].split(
    '    void PerformRequestedCacheInvalidation', 1)[0]
for statement in (
    'A64::Translate(ir_block, arch_descriptor, get_code,',
    'Optimization::Optimize(ir_block, conf, polyfill_options);',
    'const auto emitted = emitter.Emit(ir_block, false);',
):
    assert statement in block

for bits in (32, 64):
    wrapper = (generated / f'arm_dynarmic_{bits}.cpp').read_text()
    assert ('m_core_index == 0 ? 256_MiB : m_core_index < 3 ? 192_MiB : 16_MiB' if bits == 64 else
            'm_core_index == 0 ? 512_MiB : m_core_index < 3 ? 64_MiB : 16_MiB') in wrapper
    assert 'config.code_cache_size = std::uint32_t(8_MiB);' in wrapper  # Null JIT stays small.
    if not development and not diagnostics:
        for probe in ('MkwSlowRun', 'MkwRecordRunExit', 'NoteRunning', 'mkw_guest_run'):
            assert probe not in wrapper, (bits, probe)
    if development:
        assert f'EDEN_DEV_JIT bits={bits}' in wrapper
        assert 'Eden::Performance::SampleCpu' in wrapper

if not development and not diagnostics:
    for name, probes in [('k_thread.cpp', ('WaitBegin', 'WaitEnd', 'NoteRunnable')),
                         ('physical_core.cpp', ('CountIdle', 'Performance::NowNs'))]:
        source = (generated / name).read_text()
        for probe in probes:
            assert probe not in source, (name, probe)

cmake = (root / 'headless/CMakeLists.txt').read_text()
assert 'Timer read_timer' not in cmake
assert 'Timer compile_timer' not in cmake
assert 'protection_timer' not in cmake
main = (root / 'headless/main.cpp').read_text()
assert 'Performance::Reset()' not in main
assert 'Performance::Report()' not in main
assert 'Settings::values.renderer_debug = false;' in main
print('ARM32/ARM64 JIT budgets and development-only CPU sampling PASS')
