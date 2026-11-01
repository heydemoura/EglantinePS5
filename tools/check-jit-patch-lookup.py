#!/usr/bin/env python3
"""Check the generated emitter's patch lookup, linking and unlinking paths."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
source = (cache / 'native-local/headless/emit_x64.cpp').read_text()
assert 'LocationDescriptorToFriendlyName(descriptor)' not in source
methods = '\n'.join(re.search(r'void EmitX64::' + name + r'\([^\n]*\) \{.*?^}',
                             source, re.M | re.S).group()
                    for name in ('Patch', 'Unpatch'))
program = r'''
#include <cassert>
#include <cstddef>
#include <array>
#include <cstdint>
#include <memory>
using u8 = std::uint8_t; using u32 = std::uint32_t;
#include <map>
#include <vector>
namespace IR { using LocationDescriptor = unsigned; }
using CodePtr = const unsigned char*;
constexpr unsigned char arena[2048]{};
struct EmitX64 {
    struct Code {
        CodePtr cursor = arena + 999;
        unsigned moves = 0;
        CodePtr getCurr() const { return cursor; }
        CodePtr getCode() const { return arena; }
        void SetCodePtr(CodePtr value) { cursor = value; ++moves; }
    } code;
    enum class PatchKind : u32 { Jg, Jz, Jmp, MovRcx };
    struct PatchInformation { std::array<u32, 2> first{}; std::unique_ptr<std::vector<u32>> rest; };
    std::map<IR::LocationDescriptor, PatchInformation> patch_information;
    std::map<CodePtr, CodePtr> targets;
    void EmitPatchJg(unsigned, CodePtr target) { targets[code.cursor] = target; }
    void EmitPatchJz(unsigned, CodePtr target) { targets[code.cursor] = target; }
    void EmitPatchJmp(unsigned, CodePtr target) { targets[code.cursor] = target; }
    void EmitPatchMovRcx(CodePtr target) { targets[code.cursor] = target; }
    void Patch(const IR::LocationDescriptor&, CodePtr);
    void Unpatch(const IR::LocationDescriptor&);
};
METHODS
int main() {
    EmitX64 emitter;
    for (unsigned i = 0; i < 350000; ++i) emitter.Patch(i, arena + 123);
    assert(emitter.patch_information.empty());
    assert(emitter.code.moves == 0 && emitter.code.cursor == arena + 999);
    emitter.patch_information[7].first = {40, 81};
    emitter.patch_information[7].rest = std::make_unique<std::vector<u32>>(std::initializer_list<u32>{122, 163, 200});
    emitter.Patch(7, arena + 123);
    assert(emitter.targets.size() == 5 && emitter.code.cursor == arena + 999);
    for (const auto& [site, target] : emitter.targets) assert(target == arena + 123);
    emitter.Unpatch(7);
    for (const auto& [site, target] : emitter.targets) assert(target == 0);
    assert(emitter.code.cursor == arena + 999 && emitter.patch_information.size() == 1);
    emitter.Patch(7, arena + 456); // Retain incoming sites for recompilation/relinking.
    for (const auto& [site, target] : emitter.targets) assert(target == arena + 456);
    emitter.Unpatch(8);
    assert(emitter.patch_information.size() == 1 && emitter.code.cursor == arena + 999);
}
'''.replace('METHODS', methods.replace('nullptr', '0'))
with tempfile.TemporaryDirectory(prefix='eden-jit-patch-') as tmp:
    cpp = Path(tmp) / 'check.cpp'
    cpp.write_text(program)
    binary = Path(tmp) / 'check'
    subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: 350000 absent lookups create no records; all branch types link, unlink and relink')
