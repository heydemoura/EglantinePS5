#!/usr/bin/env python3
"""Exercise the real mapped-readback method with real range/writer certificates."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'Eglantine/src/video_core/renderer_vulkan/vk_buffer_cache.cpp').read_text()
start = source.index('std::span<const u8> BufferCacheRuntime::TryMappedReadback(')
end = source.index('\nvoid BufferCacheRuntime::ValidateMappedReadback', start)
method = source[start:end]
code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include "video_core/readback_write_tracker.h"
using u8=uint8_t;using u64=uint64_t;
namespace VideoCommon {struct BufferCopy{u64 src_offset,dst_offset,size;};}
bool wait_enabled{};unsigned rejection=999;
extern "C" bool eden_readback_producer_wait_enabled() __attribute__((weak));
extern "C" void eden_mapped_readback_reject(unsigned) __attribute__((weak));
extern "C" void eden_readback_producer_wait_result(u64,u64) __attribute__((weak));
extern "C" void eden_readback_producer_wait_result(u64,u64){}
extern "C" bool eden_readback_producer_wait_enabled(){return wait_enabled;}
extern "C" void eden_mapped_readback_reject(unsigned reason){rejection=reason;}
using Tracker=VideoCommon::ReadbackWriteTracker<unsigned>;
struct Scheduler {
 u64 current=9,completed{},waited{};unsigned waits{},flushes{};bool complete=true;
 std::function<void()> on_wait;
 bool IsFree(u64 tick){return tick<=completed;}
 u64 CurrentTick(){return current;}
 void Wait(u64 tick){++waits;waited=tick;if(tick>=current){++flushes;current=tick+1;}
  if(on_wait)on_wait();if(complete)completed=tick;}
};
struct Buffer {
 bool sparse{};std::array<u8,256> bytes{};std::span<const u8> readback_mapping{bytes};
 std::shared_ptr<Tracker::State> readback_writes;
 bool IsSparseCompatible()const{return sparse;}
};
struct BufferCacheRuntime {
 Scheduler scheduler;unsigned mode=2;
 unsigned MappedReadbackMode(){return mode;}
 std::span<const u8> TryMappedReadback(const Buffer&,std::span<const VideoCommon::BufferCopy>);
};
''' + method + r'''
struct Fixture {
 Tracker tracker{true};Buffer buffer;BufferCacheRuntime runtime;
 std::array<VideoCommon::BufferCopy,2> copies{{{0,0,32},{64,32,32}}};
 Fixture(){buffer.readback_writes=tracker.Register(1,256,false);
  tracker.TransferWrite(1,0,32,3,false);tracker.TransferWrite(1,64,32,5,false);wait_enabled=false;rejection=999;}
 bool read(){return !runtime.TryMappedReadback(buffer,copies).empty();}
};
int main(){
 {Fixture f;assert(!f.read()&&rejection==3&&!f.runtime.scheduler.waits);}
 {Fixture f;f.runtime.scheduler.completed=5;assert(f.read()&&!f.runtime.scheduler.waits);}
 {Fixture f;wait_enabled=true;assert(f.read());assert(f.runtime.scheduler.waited==5&&f.runtime.scheduler.waits==1);
  assert(!f.runtime.scheduler.flushes&&f.runtime.scheduler.current==9&&f.runtime.scheduler.completed==5);}
 {Fixture f;wait_enabled=true;f.runtime.scheduler.current=5;assert(f.read());assert(f.runtime.scheduler.flushes==1&&f.runtime.scheduler.current==6);}
 {Fixture f;wait_enabled=true;f.runtime.scheduler.current=4;assert(!f.read()&&rejection==2&&!f.runtime.scheduler.waits);}
 {Fixture f;wait_enabled=true;f.runtime.scheduler.on_wait=[&]{f.tracker.InvalidateAll();};assert(!f.read()&&rejection==2);}
 {Fixture f;wait_enabled=true;f.runtime.scheduler.on_wait=[&]{f.tracker.TransferWrite(1,64,32,8,false);};assert(!f.read()&&rejection==2);}
 {Fixture f;wait_enabled=true;f.runtime.scheduler.on_wait=[&]{f.tracker.NoteShaderWrite(1,0,32);};assert(!f.read()&&rejection==1);}
 {Fixture f;wait_enabled=true;f.runtime.scheduler.complete=false;assert(!f.read()&&rejection==3);}
 {Fixture f;wait_enabled=true;f.tracker.Invalidate(1);assert(!f.read()&&rejection==2&&!f.runtime.scheduler.waits);}
 {Fixture f;wait_enabled=true;f.buffer.sparse=true;assert(!f.read()&&rejection==0&&!f.runtime.scheduler.waits);}
 {Fixture f;wait_enabled=true;f.buffer.readback_mapping={};assert(!f.read()&&rejection==0&&!f.runtime.scheduler.waits);}
 {Fixture f;wait_enabled=true;f.runtime.mode=0;assert(!f.read()&&!f.runtime.scheduler.waits);}
 {Fixture f;wait_enabled=true;f.runtime.scheduler.on_wait=[&]{f.tracker.TransferWrite(1,128,32,8,false);};assert(f.read());}
 {Fixture f;wait_enabled=true;std::array<VideoCommon::BufferCopy,33> many{};
  assert(f.runtime.TryMappedReadback(f.buffer,many).empty()&&rejection==2&&!f.runtime.scheduler.waits);}
}
'''
with tempfile.TemporaryDirectory(prefix='mapped-producer-wait-') as temp:
    path = Path(temp)
    (path/'check.cpp').write_text(code)
    subprocess.run(['clang++-18','-std=c++20','-O1','-g','-pthread','-fsanitize=address,undefined',
                    '-I'+str(root/'Eglantine/src'),str(path/'check.cpp'),'-o',str(path/'check')],check=True)
    subprocess.run([str(path/'check')],check=True,timeout=10)
print('Mapped producer wait PASS: completed/current/earlier producers, one max-range wait, no later flush, stale/remap/external/unknown/sparse rejection and unrelated-range writes')
