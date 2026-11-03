#!/usr/bin/env python3
"""Exercise loading ownership and the first-guest-frame transition with real threads."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tsan',action='store_true')
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
source=(root/'headless/graphics.cpp').read_text()
assert 'std::atomic<bool> vulkan_loading{};' in source
start=source.index('bool LoadingTick(VideoCore::RendererBase& renderer, bool idle)')
end=source.index('GraphicsWindow::GraphicsWindow(',start)
assert 'RunVulkanLoadingOnOwner(split, vulkan_loading,' in source[start:end]
code=r'''
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <iostream>
#include "vulkan_loading_owner.h"
struct Split {
 static inline thread_local bool back{};
 bool active=true;std::mutex mutex;std::condition_variable work,drained;
 std::deque<std::function<void()>> queue;unsigned posted{},done{};bool stopping{};
 std::jthread thread;
 explicit Split(bool enabled=true):active(enabled),thread([this]{
  back=true;std::unique_lock lock(mutex);
  while(true){
   work.wait(lock,[&]{return stopping||!queue.empty();});
   if(queue.empty()){assert(stopping);break;}
   auto f=std::move(queue.front());queue.pop_front();lock.unlock();f();lock.lock();
   ++done;drained.notify_all();
  }
 }){}
 ~Split(){ {std::lock_guard lock(mutex);stopping=true;}work.notify_all();thread.join();}
 bool Active()const{return active;}
 static bool IsBackThread(){return back;}
 template<class F>void Defer(F f){
  static_assert(std::is_trivially_copyable_v<F>);
  static_assert(std::is_invocable_v<const F&>); // matches the real GpuSplit thunk
  {std::lock_guard lock(mutex);queue.emplace_back(f);++posted;}work.notify_one();
 }
 void Drain(){std::unique_lock lock(mutex);auto target=posted;drained.wait(lock,[&]{return done>=target;});}
};
using namespace std::chrono_literals;
int main(){
 for(bool enabled:{false,true}){
  Split split(enabled);std::atomic<bool> loading{true};unsigned calls=0;
  // Inactive mode runs on its caller. Backend mode must not recursively defer/drain.
  Split::back=enabled;
  assert(Eden::RunVulkanLoadingOnOwner(split,loading,[&]{++calls;}));
  assert(calls==1&&split.posted==0);
  loading=false;assert(!Eden::RunVulkanLoadingOnOwner(split,loading,[&]{++calls;}));assert(calls==1);
  Split::back=false;
 }
 for(bool first_guest:{false,true}){
  Split split;std::atomic<bool> loading{true},entered{},release{};
  std::atomic<unsigned> rendering{},ui_calls{};
  split.Defer([&]{
   assert(Split::IsBackThread());assert(rendering.fetch_add(1)==0);entered=true;entered.notify_all();
   release.wait(false);
   if(first_guest)loading.store(false,std::memory_order_release);
   assert(rendering.fetch_sub(1)==1);
  });
  entered.wait(false);
  auto caller=std::async(std::launch::async,[&]{return Eden::RunVulkanLoadingOnOwner(split,loading,[&]{
   assert(Split::IsBackThread());assert(rendering.fetch_add(1)==0);++ui_calls;assert(rendering.fetch_sub(1)==1);
  });});
  const auto deadline=std::chrono::steady_clock::now()+2s;
  while(true){
   {std::lock_guard lock(split.mutex);if(split.posted==2)break;}
   assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();
  }
  assert(caller.wait_for(0ms)!=std::future_status::ready&&ui_calls==0);
  release=true;release.notify_all();
  assert(caller.get()==!first_guest);assert(ui_calls==unsigned(!first_guest));
  split.Drain();assert(rendering==0);
 }
 std::cout<<"Loading owner PASS: inactive/backend calls, FIFO ownership, no concurrent renderer access, first guest frame cancels stale loading and deferred lifetime\n";
}
'''
with tempfile.TemporaryDirectory(prefix='vulkan-loading-owner-')as temporary:
 folder=Path(temporary);(folder/'check.cpp').write_text(code)
 subprocess.run(['clang++-18','-std=c++20','-O1','-g','-pthread','-fsanitize=thread' if args.tsan else '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(root/'headless'),str(folder/'check.cpp'),'-o',str(folder/'check')],check=True)
 subprocess.run([str(folder/'check')],check=True,timeout=15)
