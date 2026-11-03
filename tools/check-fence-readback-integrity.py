#!/usr/bin/env python3
"""Check fence-copy protection using actual cache/write-callback methods and a byte oracle."""
from pathlib import Path
import argparse
import subprocess,tempfile
parser=argparse.ArgumentParser(description='Exercise actual fence invalidation, CPU-write callbacks and completed writeback with owned staging memory.')
parser.add_argument('--tsan',action='store_true');args=parser.parse_args()
root=Path(__file__).resolve().parents[1];cache=Path((root/'.local/headless-cache').read_text().strip());config=(cache/'native-local/CMakeCache.txt').read_text();boost=Path(next(l.split('=',1)[1]for l in config.splitlines()if l.startswith('Boost_SOURCE_DIR:')));includes=['-I'+str(p)for p in sorted(boost.glob('libs/*/include'))];assert includes
source=(root/'Eglantine/src/video_core/buffer_cache/buffer_cache.h').read_text()
def function(text,name):
 start=text.index(name);body=text.index('{',start);end=body+1;depth=1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
methods='\n'.join('template<class P>\n'+function(source,name) for name in ['void BufferCache<P>::ClearDownload(', 'bool BufferCache<P>::OnCPUWrite(', 'void BufferCache<P>::PopAsyncBuffers('])
memory=(root/'Eglantine/src/core/memory.cpp').read_text();start=memory.index('    void HandleRasterizerWrite(');start=memory.index('[&](DAddr address) {',start)+len('[&](DAddr address) {');end=memory.index('\n        });',start);write_callback=memory[start:end]
code=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <deque>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <vector>
#include "common/common_types.h"
#include "common/range_sets.inc"
#include "video_core/buffer_cache/fence_download_invalidation.h"
#include "core/memory/rasterizer_write.h"
using Core::Memory::RecordRasterizerWrite;
namespace VideoCommon {
namespace ReadbackDependencyDiagnostics {void BufferCopyBytes(u64,u64){}}
constexpr DAddr BASE=0x1000;
struct Copy {u64 src_offset,dst_offset,size;};
struct Staging {std::span<u8> mapped_span;size_t offset{};};
template<class P>struct BufferCache {
 Common::OverlapRangeSet<DAddr> async_downloads;
 Common::RangeSet<DAddr> uncommitted_gpu_modified_ranges,gpu_modified_ranges;
 std::deque<Common::RangeSet<DAddr>> committed_gpu_modified_ranges;
 std::deque<std::vector<Copy>> pending_downloads;
 std::deque<std::optional<Staging>> async_buffers;
 std::vector<Staging> async_buffers_death_ring;
 struct Memory {std::array<u8,64> guest{};void WriteBlockUnsafe(DAddr at,const u8* data,u64 size){assert(at>=BASE&&at+size<=BASE+64);std::copy_n(data,size,guest.begin()+(at-BASE));}} device_memory;
 struct Tracker {bool IsRegionGpuModified(DAddr,u64){return true;}} memory_tracker;
 bool IsRegionRegistered(DAddr,u64){return true;}
 void WriteMemory(DAddr at,u64 size){ClearDownload(at,size);gpu_modified_ranges.Subtract(at,size);}
 void ClearDownload(DAddr,u64);bool OnCPUWrite(DAddr,u64);void PopAsyncBuffers();
 void Fence(std::span<u8> values){pending_downloads.push_back({{BASE,0,values.size()}});async_buffers.push_back(Staging{values});async_downloads.Add(BASE,values.size());gpu_modified_ranges.Add(BASE,values.size());}
};
'''+methods+r'''
}
constexpr unsigned YUZU_PAGEBITS=12;
struct Writes {
 struct State {DAddr last_address{};};std::array<State,6> rasterizer_write_areas{};
 struct GPU {unsigned calls{};bool OnCPUWrite(DAddr,size_t){++calls;return true;}} gpu;
 struct System {GPU& gpu;GPU& GPU(){return gpu;}}system{gpu};
 struct Dirty {unsigned calls{};void Collect(DAddr,size_t){++calls;}};
 std::array<Dirty,6> gpu_dirty_managers;
 void Written(DAddr address,size_t size){const size_t core=0;
'''+write_callback+r'''
 }
};
int main(){
 unsigned failures=0;
 auto check=[&](bool condition,const char* text){if(!condition){++failures;std::cerr<<"CONTRACT FAILED: "<<text<<'\n';}};
 using Cache=VideoCommon::BufferCache<void>;using VideoCommon::BASE;
 std::array<u8,64> old,newer;old.fill(0xa1);newer.fill(0xc3);
 {
  Cache cache;cache.Fence(old);check(cache.OnCPUWrite(BASE+8,8),"GPU-modified write still collected");
  std::fill_n(cache.device_memory.guest.begin()+8,8,u8{0xb2});cache.PopAsyncBuffers();
  for(unsigned i=8;i<16;++i)check(cache.device_memory.guest[i]==0xb2,"CPU bytes survive older fence completion");
 }
 {
  Cache cache;cache.Fence(old);cache.ClearDownload(BASE,64);cache.device_memory.guest.fill(0xb2);cache.Fence(newer);
  cache.PopAsyncBuffers();check(cache.device_memory.guest[0]==0xb2,"old fence cannot consume a newer fence's overlap count");
  cache.PopAsyncBuffers();check(cache.device_memory.guest==newer,"newest fence data reaches guest memory");
  check(cache.async_downloads.Empty(),"all completed fence counts retire");
 }
 {
  Writes writes;writes.Written(BASE+8,8);writes.Written(BASE+24,8);
  check(writes.gpu.calls==2,"every same-page write reaches OnCPUWrite for range-local pending-fence protection");
  check(writes.gpu_dirty_managers[0].calls==2,"both writes are collected");
 }
 if(!failures){
  Cache cache;
  struct Reference {std::array<u8,64> bytes;std::array<bool,64> valid;};
  std::deque<std::array<u8,64>> staging;std::deque<Reference> pending;
  std::array<u8,64> expected{};std::mt19937 rng{842739};
  auto compare=[&]{
   assert(cache.device_memory.guest==expected);
   std::array<s32,64> counts{},wanted{};
   for(const auto& fence:pending)for(unsigned i=0;i<64;++i)wanted[i]+=fence.valid[i];
   cache.async_downloads.ForEachInRange(BASE,64,[&](DAddr start,DAddr end,s32 count){
    for(auto at=start;at<end;++at)counts[at-BASE]=count;
   });
   assert(counts==wanted);
  };
  auto pop=[&]{
   for(unsigned i=0;i<64;++i)if(pending.front().valid[i])expected[i]=pending.front().bytes[i];
   pending.pop_front();cache.PopAsyncBuffers();compare();
  };
  for(unsigned step=0;step<20000;++step){
   const unsigned choice=rng()%4;
   if(pending.size()>=8||(!pending.empty()&&choice==0)){pop();}
   else if(pending.empty()||choice==1){
    staging.emplace_back();for(auto& value:staging.back())value=static_cast<u8>(rng());
    Reference fence{staging.back(),{}};fence.valid.fill(true);pending.push_back(fence);
    cache.Fence(staging.back());compare();
   }else{
    const unsigned start=rng()%64,length=1+rng()%(64-start);const u8 value=static_cast<u8>(rng());
    assert(cache.OnCPUWrite(BASE+start,length));
    for(auto& fence:pending)std::fill_n(fence.valid.begin()+start,length,false);
    std::fill_n(expected.begin()+start,length,value);
    std::fill_n(cache.device_memory.guest.begin()+start,length,value);compare();
   }
  }
  while(!pending.empty())pop();
  assert(cache.async_downloads.Empty());
  Writes writes;writes.Written(0,8);writes.Written(BASE,0);assert(!writes.gpu.calls&&!writes.gpu_dirty_managers[0].calls);
  std::cout<<"20,000-event byte oracle PASS: partial CPU writes, overlapping fences, staging offsets, per-fence counts, ordered retirement and zero writes\n";
 }
 std::cout<<"Fence integrity contract violations: "<<failures<<'\n';return failures?1:0;
}
'''
with tempfile.TemporaryDirectory(prefix='fence-integrity-')as temp:
 folder=Path(temp);(folder/'check.cpp').write_text(code)
 subprocess.run(['clang++-18','-std=c++20','-O1','-g','-pthread','-fsanitize=thread' if args.tsan else '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(root/'Eglantine/src'),*includes,str(folder/'check.cpp'),'-o',str(folder/'check')],check=True)
 subprocess.run([str(folder/'check')],check=True)
