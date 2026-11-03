#!/usr/bin/env python3
"""Exercise actual texture-retirement queues, policy helper and Vulkan tick hooks."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tsan',action='store_true')
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
core=root/'Eglantine/src/video_core'
source=(core/'texture_cache/texture_cache.h').read_text()
# Pending downloads retain independent ownership; immediate-delete callers stay distinct.
assert 'else if (!immediate_delete) {\n        RetireTextureObject(runtime, sentenced_images, completed_images,' in source
assert source.count('RetireTextureObject(')==4
prefix=source[source.index('void TextureCache<P>::TickFrame() {'):]
assert prefix.index('completed_framebuffers.Collect') < prefix.index('completed_image_views.Collect') < prefix.index('completed_images.Collect') < prefix.index('RunGarbageCollector();')
main=(root/'headless/main.cpp').read_text()
boot_start=main.index('        // Owned boot selection for submission-completion texture retirement.')
boot_end=main.index('        // Enable bounded readback probes for explicit diagnostic builds.',boot_start)
assert main.rfind('#ifdef EDEN_MKW_DIAGNOSTICS',0,boot_start) > main.rfind('#endif',0,boot_start)
assert boot_end < main.index('Common::Log::Initialize();',boot_end)
boot_selection=main[boot_start:boot_end]
runtime_source=(core/'renderer_vulkan/vk_texture_cache.cpp').read_text()
runtime_hooks=runtime_source[runtime_source.index('u64 TextureCacheRuntime::RetirementTick() const {'):runtime_source.index('void TextureCacheRuntime::TickFrame() {')]
code=r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include <map>
#include <random>
#include <thread>
#include <vector>
#include "video_core/delayed_destruction_ring.h"
#include "video_core/texture_cache/completion_destruction.h"
using u64=std::uint64_t;
struct Scheduler {
 u64 tick=1,known=0;
 u64 CurrentTick()const{return tick;}
 bool IsFree(u64 value)const{return known>=value;}
};
struct TextureCacheRuntime {
 Scheduler& scheduler;
 u64 RetirementTick() const;
 bool IsRetirementComplete(u64) const;
};
'''+runtime_hooks+r'''
namespace Eden {
std::string boot_path;
std::string AppFile(const char*) {return boot_path;}
namespace BootTrace {void Line(const char*,unsigned) {}}
}
void ConfigureBoot() {
'''+boot_selection+r'''
}
using namespace VideoCommon;
struct Resource {
 unsigned id{};static inline std::array<unsigned,100000> deaths{};
 explicit Resource(unsigned value):id(value){}
 Resource(const Resource&)=delete;
 Resource(Resource&& other)noexcept:id(std::exchange(other.id,0)){}
 ~Resource(){if(id){assert(deaths[id]++==0);}}
};
struct NoHooks {};
int main(int argc,char**argv){
 assert(argc==2||argc==4);bool enabled=std::atoi(argv[1]);
 if(argc==4){
  Eden::boot_path=argv[3];
  if(std::string(argv[2])!="missing"){std::ofstream file(Eden::boot_path);file<<argv[2];}
  // An inherited/stale environment value must not override a missing or invalid control.
  setenv("EDEN_PS5_COMPLETION_RETIREMENT","1",1);ConfigureBoot();
  assert(std::string(std::getenv("EDEN_PS5_COMPLETION_RETIREMENT"))==(enabled?"1":"0"));
  std::remove(Eden::boot_path.c_str());
 }else if(enabled)setenv("EDEN_PS5_COMPLETION_RETIREMENT","1",1);
 else unsetenv("EDEN_PS5_COMPLETION_RETIREMENT");
 Scheduler scheduler;TextureCacheRuntime runtime{scheduler};
 {
  DelayedDestructionRing<Resource,8> ring;CompletionDestructionQueue<Resource> queue;
  RetireTextureObject(runtime,ring,queue,Resource{1});
  for(unsigned i=0;i<32;++i){ring.Tick();queue.Collect([&](u64 t){return runtime.IsRetirementComplete(t);});}
  // The current tick is not submitted/completed. Frame age cannot release it in completion mode.
  assert(Resource::deaths[1]==unsigned(!enabled));
  scheduler.known=1;queue.Collect([&](u64 t){return runtime.IsRetirementComplete(t);});
  assert(Resource::deaths[1]==1);
 }
 {
  // Backends without submission hooks always retain their frame policy.
  NoHooks fallback;DelayedDestructionRing<Resource,8> ring;CompletionDestructionQueue<Resource> queue;
  RetireTextureObject(fallback,ring,queue,Resource{2});
  for(unsigned i=0;i<7;++i)ring.Tick();assert(!Resource::deaths[2]);
  ring.Tick();assert(Resource::deaths[2]==1);
 }
 {
  CompletionDestructionQueue<Resource> queue;
  queue.Push(10,Resource{3});queue.Push(10,Resource{4});queue.Push(11,Resource{5});
  unsigned count=0;queue.ForEach([&](const Resource&){++count;});assert(count==3);
  queue.Collect([](u64 t){return t<=9;});assert(!Resource::deaths[3]&&!Resource::deaths[4]);
  queue.Collect([](u64 t){return t<=10;});assert(Resource::deaths[3]&&Resource::deaths[4]&&!Resource::deaths[5]);
  queue.Collect([](u64 t){return t<=11;});assert(Resource::deaths[5]);
 }
 {
  // A queued recording worker may still own the current tick. Completion is published
  // by a separate mock GPU thread only after that worker's work reaches the GPU.
  CompletionDestructionQueue<Resource> queue;std::atomic<bool> release{},completed{};
  std::jthread gpu([&]{release.wait(false);completed.store(true,std::memory_order_release);completed.notify_one();});
  queue.Push(12,Resource{6});queue.Collect([&](u64){return completed.load(std::memory_order_acquire);});
  assert(!Resource::deaths[6]);release=true;release.notify_one();completed.wait(false);
  queue.Collect([&](u64){return completed.load(std::memory_order_acquire);});assert(Resource::deaths[6]);
 }
 {
  // Independent per-resource reference model checks multiple objects/tick, delayed
  // completion, long periods without completion and release of every object exactly once.
  CompletionDestructionQueue<Resource> queue;std::map<unsigned,u64> expected;
  std::mt19937 rng(0x29c054);u64 current=20,done=19;
  for(unsigned id=10;id<20010;++id){
   current+=rng()%3==0;queue.Push(current,Resource{id});expected[id]=current;
   if(rng()%7==0)done+=rng()%static_cast<unsigned>(current-done+1);
   queue.Collect([&](u64 tick){return tick<=done;});
   for(auto it=expected.begin();it!=expected.end();){
    if(it->second<=done){assert(Resource::deaths[it->first]==1);it=expected.erase(it);}
    else {assert(!Resource::deaths[it->first]);++it;}
   }
   unsigned count=0;queue.ForEach([&](const Resource& r){assert(expected.contains(r.id));++count;});
   assert(count==expected.size());
  }
  queue.Collect([&](u64 tick){return tick<=current;});
  for(unsigned id=10;id<20010;++id)assert(Resource::deaths[id]==1);
 }
 std::cout<<"Texture retirement PASS mode="<<enabled<<": default/fallback eight-frame policy, unsubmitted worker ownership, per-tick release, move-only lifetimes and 20000-object oracle\n";
}
'''
with tempfile.TemporaryDirectory(prefix='texture-retirement-')as temporary:
 folder=Path(temporary);(folder/'check.cpp').write_text(code)
 subprocess.run(['clang++-18','-std=c++20','-O1','-g','-pthread','-fsanitize=thread' if args.tsan else '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(root/'Eglantine/src'),str(folder/'check.cpp'),'-o',str(folder/'check')],check=True)
 for mode in ['0','1']:subprocess.run([str(folder/'check'),mode],check=True,timeout=15)

 for expected,content in [('1','1\n'),('0','missing'),('0','0'),('0','2'),('0','-1'),('0','bad'),('0','1 0'),('0','999999999999999999999999')]:
  subprocess.run([str(folder/'check'),expected,content,str(folder/'mode.txt')],check=True,timeout=15)
