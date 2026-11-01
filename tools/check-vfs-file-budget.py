#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run real VFS eviction/reopen methods under a smaller native file budget."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
adapted = (cache / 'native-local/headless/vfs_real.cpp').read_text()
upstream = (root / 'Eglantine/src/core/file_sys/vfs/vfs_real.cpp').read_text()

def policy(source):
    start = source.index('size_t MaxOpenFiles() {')
    return source[start:source.index('\n}\n', start) + 3]

start = adapted.index('std::unique_lock<std::mutex> RealVfsFilesystem::RefreshReference(')
methods = adapted[start:adapted.index('\nRealVfsFile::RealVfsFile(', start)]
program = r'''
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <sys/resource.h>
#include "common/intrusive_list.h"
size_t live_files = 0, peak_files = 0;
namespace Common::FS {
struct IOFile {
    FILE* handle;
    IOFile(const std::string& path) : handle(std::fopen(path.c_str(), "rb")) {
        if (handle) peak_files = std::max(peak_files, ++live_files);
    }
    ~IOFile() { if (handle) { std::fclose(handle); --live_files; } }
};
enum class FileType { BinaryFile };
std::shared_ptr<IOFile> FileOpen(const std::string& path, int, FileType) {
    auto file = std::make_shared<IOFile>(path);
    return file->handle ? file : nullptr;
}
}
// PS5 resource exhaustion happens below its reported RLIMIT_NOFILE. Keep a
// real low descriptor ceiling, but report the larger limit to the VFS policy.
int ReportedLimit(int, struct rlimit* limit) { limit->rlim_cur = 8192; return 0; }
#define getrlimit ReportedLimit
POLICY
#undef getrlimit
namespace FileSys {
namespace FS = Common::FS;
enum class OpenMode { Read };
int ModeFlagsToFileAccessMode(OpenMode) { return 0; }
struct FileReference : Common::IntrusiveListBaseNode<FileReference> {
    std::shared_ptr<FS::IOFile> file;
};
struct RealVfsFilesystem {
    using List = Common::IntrusiveListBaseTraits<FileReference>::ListType;
    List open_references, closed_references;
    std::mutex list_lock;
    size_t num_open_files = 0;
    bool in_dtor = false;
    std::unique_lock<std::mutex> RefreshReference(const std::string&, OpenMode, FileReference&);
    void DropReference(std::unique_ptr<FileReference>&&);
    void EvictSingleReferenceLocked();
    void InsertReferenceIntoListLocked(FileReference&);
    void RemoveReferenceFromListLocked(FileReference&);
};
METHODS
}
int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    const bool expect_exhaustion = argc == 3;
    struct rlimit limit;
    assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    limit.rlim_cur = 256;
    assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    std::vector<FILE*> reserved;
    for (int i = 0; i < 64; ++i) { auto f = std::fopen(argv[1], "rb"); assert(f); reserved.push_back(f); }
    FileSys::RealVfsFilesystem fs;
    std::vector<std::unique_ptr<FileSys::FileReference>> refs;
    bool exhausted = false;
    for (int i = 0; i < 600; ++i) {
        auto& ref = refs.emplace_back(std::make_unique<FileSys::FileReference>());
        fs.InsertReferenceIntoListLocked(*ref);
        auto lock = fs.RefreshReference(argv[1], FileSys::OpenMode::Read, *ref);
        if (!ref->file) { exhausted = true; break; }
        assert(std::fgetc(ref->file->handle) == 'X');
    }
    assert(exhausted == expect_exhaustion);
    if (!exhausted) {
        assert(peak_files <= 128 && !refs.front()->file);
        auto lock = fs.RefreshReference(argv[1], FileSys::OpenMode::Read, *refs.front());
        assert(refs.front()->file && std::fgetc(refs.front()->file->handle) == 'X');
    }
    for (auto& ref : refs) { fs.DropReference(std::move(ref)); ref.reset(); }
    assert(live_files == 0 && fs.num_open_files == 0);
    for (auto file : reserved) std::fclose(file);
}
'''.replace('METHODS', methods)
with tempfile.TemporaryDirectory(prefix='eden-vfs-budget-') as temporary:
    folder = Path(temporary)
    (folder / 'common').mkdir()
    (folder / 'common/assert.h').write_text('#pragma once\n#include <cassert>\n#include <cstdlib>\n#define ASSERT(x) assert(x)\n#define UNREACHABLE() std::abort()\n')
    data = folder / 'data'
    data.write_text('X')
    for name, source, args in [('ps5', adapted, []), ('upstream-control', upstream, ['expect-exhaustion'])]:
        cpp, executable = folder / (name + '.cpp'), folder / name
        cpp.write_text(program.replace('POLICY', policy(source)))
        subprocess.run(['g++', '-std=c++20', '-pthread', '-I' + str(folder),
                        '-I' + str(root / 'Eglantine/src'), str(cpp), '-o', str(executable)], check=True)
        subprocess.run([str(executable), str(data), *args], check=True)
print('VFS file budget: 600 references, real descriptor pressure, LRU reopen/cleanup and upstream exhaustion control PASS')
