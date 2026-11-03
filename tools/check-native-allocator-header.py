#!/usr/bin/env python3
"""Reject stale cached class layouts in the native Vulkan factory and allocator users."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
cache=Path((root/'.local/headless-cache').read_text().strip())/'native-local'
expected=cache/'headless/include/video_core/vulkan_common/vulkan_memory_allocator.h'
old=cache.parent/'source/src/video_core/vulkan_common/vulkan_memory_allocator.h'
for source in ['video_core.cpp','vulkan_renderer.cpp','vulkan_memory_allocator.cpp','vk_texture_cache_cost.cpp']:
    obj='src/video_core/CMakeFiles/video_core.dir/__/__/headless/'+source+'.o'
    output=subprocess.check_output(['ninja','-C',str(cache),'-t','deps',obj],text=True)
    headers={line.strip() for line in output.splitlines()[1:]}
    assert str(expected) in headers and str(old) not in headers, (source,'stale allocator layout; rebuild with CCACHE_NODIRECT=1')
print('Native allocator layouts PASS: factory, renderer, allocator and texture cache use the derived header')
