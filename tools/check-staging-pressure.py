#!/usr/bin/env python3
"""Exercise the generated staging-cache reclamation with outstanding references."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
source = (cache / 'native-local/headless/vulkan_staging_buffer_pool.cpp').read_text()
start = source.index('void StagingBufferPool::ReleaseCompleted() {')
end = source.index('\nvoid StagingBufferPool::TickFrame()', start)
body = source[start:end]
code = r'''
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "staging_buffers.h"
struct StagingBufferPool {
    struct StagingBuffer {
        unsigned index;
        bool deferred;
        std::uint64_t tick;
        std::span<std::byte> mapped_span{};
        unsigned log2_level = 24;
    };
    struct StagingBuffers {
        std::vector<StagingBuffer> entries;
        std::size_t iterate_index = 0, delete_index = 0;
    };
    using StagingBuffersCache = std::array<StagingBuffers, 3>;
    struct Scheduler {
        std::uint64_t completed = 10;
        bool IsFree(std::uint64_t tick) const { return tick <= completed; }
    } scheduler;
    StagingBuffersCache device_local_cache, upload_cache, download_cache;
    void ReleaseCompleted();
};
''' + body + r'''
int main() {
    StagingBufferPool pool;
    for (auto* cache : {&pool.device_local_cache, &pool.upload_cache, &pool.download_cache}) {
        for (auto& level : *cache) {
            // A deferred reference must survive even when its old tick is complete.
            level.entries = {{1, false, 9}, {2, true, 1}, {3, false, 11}, {4, false, 10}};
            level.iterate_index = 4;
            level.delete_index = 3;
        }
    }
    pool.ReleaseCompleted();
    for (auto* cache : {&pool.device_local_cache, &pool.upload_cache, &pool.download_cache}) {
        for (auto& level : *cache) {
            assert(level.entries.size() == 2);
            assert(level.entries[0].index == 2 && level.entries[1].index == 3);
            assert(level.iterate_index <= level.entries.size());
            assert(level.delete_index < level.entries.size());
        }
    }
    pool.ReleaseCompleted(); // repeated pressure cannot release outstanding work
    pool.scheduler.completed = 11;
    pool.ReleaseCompleted();
    for (auto* cache : {&pool.device_local_cache, &pool.upload_cache, &pool.download_cache}) {
        for (auto& level : *cache) {
            assert(level.entries.size() == 1 && level.entries[0].index == 2);
            level.entries[0].deferred = false;
        }
    }
    pool.ReleaseCompleted();
    for (auto* cache : {&pool.device_local_cache, &pool.upload_cache, &pool.download_cache}) {
        for (auto& level : *cache) {
            assert(level.entries.empty());
            assert(level.iterate_index == 0 && level.delete_index == 0);
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix='eglantine-staging-pressure-') as directory:
    source_path = Path(directory) / 'check.cpp'
    executable = Path(directory) / 'check'
    source_path.write_text(code)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(root / 'headless'),
                    str(source_path), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
print('Staging pressure: PASS completed-only reclamation, deferred/in-flight references, cache cursors and repeated cleanup')
