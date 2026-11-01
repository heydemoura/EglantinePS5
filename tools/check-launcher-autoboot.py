#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual development selection branch, including a missing title."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'headless/main.cpp').read_text()
start = source.index('        if (std::exchange(autoboot_pending, false)) {')
end = source.index('#else\n        Eden::BootTrace::Line("opening the launcher");', start)
branch = source[start:end]
program = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
struct Entry {
    std::filesystem::path name;
    const std::filesystem::path& path() const { return name; }
};
std::vector<Entry> entries;
std::uint64_t metadata;
int launcher_calls;
std::string launcher_choice;
namespace Eden {
std::string AssetsPath(const char*) { return "/roms"; }
std::vector<Entry> ReadNativeDirectory(const std::string& folder, std::error_code&) {
    return folder == "/roms" ? entries : std::vector<Entry>{};
}
namespace BootTrace { template <typename... T> void Line(const char*, T...) {} }
}
std::uint64_t eden_game_title_id(const char*) { return metadata; }
std::string SelectEglantinePS5Game(int, const std::string&) {
    ++launcher_calls;
    return launcher_choice;
}
struct Result { std::string selected, error; bool pending; };
Result Select(bool autoboot_pending) {
    int rooms = 0;
    std::string development_id = "040074A01BF12000", selected_game, launch_error;
BRANCH
    return {selected_game, launch_error, autoboot_pending};
}
int main() {
    launcher_choice = "/roms/chosen.xci";
    metadata = 0;
    for (auto candidates : {std::vector<Entry>{}, std::vector<Entry>{{"LooseGame"}, {"other.xci"}}}) {
        entries = candidates;
        launcher_calls = 0;
        auto result = Select(true);
        assert(result.selected == launcher_choice && !result.error.empty());
        assert(launcher_calls == 1 && !result.pending);
    }
    entries = {{"040074A01BF12000.nsp"}};
    launcher_calls = 0;
    auto named = Select(true);
    assert(named.selected == "/roms/040074A01BF12000.nsp" && named.error.empty());
    assert(launcher_calls == 0 && !named.pending);
    entries = {{"named.xci"}};
    metadata = 0x040074A01BF12000ULL;
    auto matched = Select(true);
    assert(matched.selected == "/roms/named.xci" && matched.error.empty());
    assert(launcher_calls == 0);
    auto first = Select(false);
    assert(first.selected == launcher_choice && first.error.empty() && launcher_calls == 1);
    launcher_choice.clear();
    auto quit = Select(false);
    assert(quit.selected.empty() && quit.error.empty() && launcher_calls == 2);
}
'''.replace('BRANCH', branch)
with tempfile.TemporaryDirectory(prefix='eden-launcher-autoboot-') as temporary:
    folder = Path(temporary)
    cpp, executable = folder / 'test.cpp', folder / 'test'
    cpp.write_text(program)
    subprocess.run(['g++', '-std=c++20', str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('Launcher startup: missing title fallback, filename/metadata autoboot, launcher-first and quit PASS')
