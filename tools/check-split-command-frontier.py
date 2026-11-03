#!/usr/bin/env python3
"""Exercise actual split ring/dispatch/wait methods and the bounded fetch observer."""
from pathlib import Path
import argparse
import subprocess
import re
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tsan', action='store_true', help='use ThreadSanitizer instead of ASan/UBSan')
args = parser.parse_args()

root = Path(__file__).resolve().parents[1]
source = (root/'Eglantine/src/video_core/gpu_split.cpp').read_text()

def method(name, text=source):
    start = text.index(name)
    body = text.index('{', start)
    end, depth = body+1, 1
    while depth:
        depth += (text[end] == '{')-(text[end] == '}')
        end += 1
    return text[start:end]

methods = '\n'.join(method(name) for name in [
    'void GpuSplit::Stop()', 'u64* GpuSplit::ReserveSlow(',
    'void GpuSplit::WaitForSpace(', 'void GpuSplit::Publish()',
    'void GpuSplit::Drain()', 'void GpuSplit::DrainCommandReads()',
    'void GpuSplit::DrainTo(', 'void GpuSplit::SwitchEngine(',
    'void GpuSplit::MultiMethod(', 'size_t GpuSplit::Execute(',
    'void GpuSplit::BackLoop('])
front = (root/'Eglantine/src/video_core/engines/maxwell_3d.cpp').read_text()
classify_start = front.index('        const bool may_write = VideoCommon::CommandWriteFrontier::StateWriteMayExecute(')
classify_end = front.index(';', front.index('split.MacroWrite(this, method, argument, may_write)', classify_start))+1
classification = front[classify_start:classify_end]
# Compile the real compute frontend call sites, native store-only backend prefix,
# and constructor table. Test scaffolding replaces heavyweight launch/macro work
# only after the real store-only early return; it does not duplicate the table.
compute = (root/'Eglantine/src/video_core/engines/kepler_compute.cpp').read_text()
compute_header = (root/'Eglantine/src/video_core/engines/kepler_compute.h').read_text()
positions = dict(re.findall(r'ASSERT_REG_POSITION\((\w+), (0x[0-9A-Fa-f]+)\);', compute_header))
compute_constants = '\n'.join(f'constexpr u32 reg_{name}={positions[name]};' for name in ['exec_upload','data_upload','launch'])
compute_numregs = re.search(r'NUM_REGS = (0x[0-9A-Fa-f]+);', compute_header)[1]
table = compute[compute.index('    native_plain_methods.set();'):compute.index('\n}\n\nKeplerCompute::~')]
compute_calls = '\n'.join(method(name,compute) for name in [
    'void KeplerCompute::FrontCallMethod(', 'void KeplerCompute::FrontCallMultiMethod('])
backend = method('void KeplerCompute::CallMethod(',compute)
backend = backend[:backend.index('        if (ProcessNativeMethod(')] + '    }\n    ++fallback_calls;\n}'
emit_start = compute.index('        [&](u32 address, u32 value) {', compute.index('void KeplerCompute::FrontRunMacro('))
emit_end = compute.index('        });',emit_start)
emit = compute[compute.index('{',emit_start)+1:emit_end]  # Body only; the fixture supplies the closing brace.
compute_fixture = r'''
#define KEPLER_COMPUTE_REG_INDEX(name) reg_ ## name
#define LOG_ERROR(...) ((void)0)
''' + compute_constants + r'''
struct Compute:Tegra::Engines::EngineInterface {
 struct Regs {static constexpr size_t NUM_REGS=''' + compute_numregs + r''';std::array<u32,NUM_REGS> reg_array{};} regs;
 struct FrontState {std::array<u32,0x1000> registers{};std::optional<u32> executing_macro;std::vector<u32> parameters;} fs;
 bool native_mode{};std::bitset<0xe00> native_plain_methods;
 std::array<u32,0x1000> native_registers{};unsigned fallback_calls{},macro_calls{};
 void Configure(bool native) {native_mode=native;execution_mask.set();
''' + table + r'''
 }
 FrontState& Front(Split&){return fs;}
 void FrontApply(FrontState& state,u32 method,u32 value){state.registers[method]=value;}
 void FrontRunMacro(Core::System&,Split&,FrontState&){++macro_calls;}
 void FrontCallMethod(Core::System&,Split&,u32,u32,bool,const GPUVAddr*);
 void FrontCallMultiMethod(Core::System&,Split&,u32,const u32*,u32,u32,const GPUVAddr*);
 void CallMethod(Core::System&,u32,u32,bool)override;
 void CallMultiMethod(Core::System& system,u32 method,const u32* values,u32 amount,u32 pending)override{
  for(u32 i=0;i<amount;++i)CallMethod(system,method,values[i],pending-i<=1);
 }
 bool Emit(Split& split,u32 address,u32 value){auto& fs=this->fs;
''' + emit + r'''
 }
};
''' + compute_calls.replace('KeplerCompute::','Compute::') + '\n' + backend.replace('KeplerCompute::','Compute::')
header = (root/'Eglantine/src/video_core/gpu_split.h').read_text()
assert header.count('private:') == 1
header = header.replace('private:', 'public:')  # Test access only, never the compiled port.
code = r'''
#include <cassert>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <future>
#include <iostream>
#include <optional>
#include "common/common_types.h"
#include "video_core/nx2.h"
namespace Core {struct System {void RegisterHostThread(){}};}
#include "split-under-test.h"
namespace Common {
enum class ThreadPriority {Critical};
void SetCurrentThreadName(const char*){}
void SetCurrentThreadPriority(ThreadPriority){}
void SetCurrentThreadToPerformanceCores(){}
}
namespace VideoCommon::Nx2Profile {
enum class Zone {SplitDrain,SplitWait,Method};
struct Scope {explicit Scope(Zone){}};
void Report(const char* = nullptr){}
}
std::atomic<bool> narrow{true};
std::atomic<unsigned> idle_spins{8192};
extern "C" unsigned eden_split_back_idle_spins() __attribute__((weak));
extern "C" unsigned eden_split_back_idle_spins(){return idle_spins.load();}
std::atomic<unsigned> trace_begins{},trace_ends{};
extern "C" bool eden_split_fetch_frontier_enabled() __attribute__((weak));
extern "C" u64 eden_split_fetch_begin(u64,u64,u64,unsigned) __attribute__((weak));
extern "C" void eden_split_fetch_end(u64) __attribute__((weak));
extern "C" bool eden_split_fetch_frontier_enabled(){return narrow.load();}
extern "C" u64 eden_split_fetch_begin(u64 target,u64 queued,u64 consumed,unsigned mode){
 assert(target<=queued&&consumed<=queued&&mode==unsigned(narrow.load()));
 if(!mode)assert(target==queued);
 ++trace_begins;return 0;
}
extern "C" void eden_split_fetch_end(u64 index){assert(index==0);++trace_ends;}
namespace VideoCommon {
static std::atomic<bool> g_back_busy{};
static void Pause(){std::this_thread::yield();}
GpuSplit GpuSplit::instance;
''' + methods + r'''
}
using Split=VideoCommon::GpuSplit;
using Clock=std::chrono::steady_clock;
bool await(std::atomic<bool>& flag){
 auto end=Clock::now()+std::chrono::seconds(3);
 while(!flag.load(std::memory_order_acquire)){
  if(Clock::now()>end)return false;
  std::this_thread::yield();
 }
 return true;
}
struct Engine:Tegra::Engines::EngineInterface {
 struct FrontState {bool draw_pending;};
 void Emit(Split& split,FrontState& fs,u32 method,u32 argument){
''' + classification + r'''
 }
 std::atomic<bool> tail_started{},tail_release{};
 std::atomic<unsigned> state{};
 void CallMethod(Core::System&,u32 method,u32 value,bool)override{
  if(method==7){tail_started.store(true,std::memory_order_release);await(tail_release);}
  state.store(value,std::memory_order_release);
 }
 void CallMultiMethod(Core::System& system,u32 method,const u32* values,u32 count,u32)override{
  for(u32 i=0;i<count;++i)CallMethod(system,method,values[i],true);
 }
};
void start(Split& s,Core::System& system){
 s.system=&system;s.active=true;s.ring=std::make_unique<u64[]>(s.RingWords);
 s.back_thread=std::jthread([&s](std::stop_token stop){s.BackLoop(stop);});
}
''' + compute_fixture + r'''
int main(){
 Core::System system;
 for(unsigned budget:{0U,128U,8192U,16384U})for(bool enabled:{false,true}){
  idle_spins.store(budget);narrow.store(enabled);Split s;Engine engine;
  std::atomic<bool> writer_started{},writer_release{};
  std::atomic<unsigned> guest{};
  constexpr GPUVAddr address=0x50000;
  Tegra::Nx2ClearGpuWrittenPages();
  start(s,system);
  s.Defer([&]{writer_started.store(true,std::memory_order_release);await(writer_release);
              guest.store(42,std::memory_order_release);Tegra::Nx2MarkGpuWritten(address,4);});
  const u64 producer=s.command_writes.Target();assert(producer>0);
  // This tail only touches engine state. Block it to make early producer completion observable.
  s.MacroWrite(&engine,7,99,false);const u64 queued=s.write_position;
  assert(producer<queued&&s.command_writes.Target()==producer);
  s.Publish();assert(await(writer_started));
  auto waiter=std::async(std::launch::async,[&]{
   s.DrainCommandReads();
   // The first queued writer must have run before historical-page inspection.
   assert(guest.load(std::memory_order_acquire)==42);
   assert(Tegra::Nx2HasGpuWrittenPage(address,4,[](GPUVAddr page){return std::optional{page};}));
  });
  // Observe the actual completion request before releasing the producer. If a
  // request arrives after the back passed it, acknowledgement can conservatively
  // wait until the next published boundary; that timing is not an ordering bypass.
  const auto request_deadline=Clock::now()+std::chrono::seconds(2);
  while(s.completion.requested.load()!= (enabled?producer:queued) && Clock::now()<request_deadline)
   std::this_thread::yield();
  const bool requested=s.completion.requested.load()==(enabled?producer:queued);
  writer_release.store(true,std::memory_order_release);assert(await(engine.tail_started));
  const bool returned=waiter.wait_for(std::chrono::milliseconds(200))==std::future_status::ready;
  engine.tail_release.store(true,std::memory_order_release);
  waiter.get();s.Drain();
  assert(requested&&returned==enabled); // OFF waits for the entire tail; ON acknowledges its actual producer.
  assert(engine.state.load()==99&&s.consumed.load()==queued);
  s.Stop();assert(!s.active&&s.command_writes.Target()==0&&s.write_position==0);
  assert(!s.completion.ShouldPublish(1,0));
 }
 // Classification is conservative by default, including arbitrary deferred callbacks.
 {
  Split s;Engine e;s.ring=std::make_unique<u64[]>(s.RingWords);
  s.Method(&e,5,1,true,nullptr);assert(s.command_writes.Target()==0);
  e.execution_mask[6]=true;s.Method(&e,6,1,true,nullptr);
  assert(s.command_writes.Target()==s.write_position);
  s.MacroWrite(&e,5,2);assert(s.command_writes.Target()==s.write_position);
  s.Defer([]{});assert(s.command_writes.Target()==s.write_position);
  const auto callback=s.command_writes.Target();
  for(unsigned i=0;i<100;++i)s.MacroWrite(&e,5,i,false);
  assert(s.command_writes.Target()==callback);
  const u32 values[]{1,2,3};s.MultiMethod(&e,5,values,3,3,nullptr);
  assert(s.command_writes.Target()==s.write_position);
 }
 // Exercise the actual FrontWrite call-site classification against the actual ring.
 {
  Split s;Engine e;s.ring=std::make_unique<u64[]>(s.RingWords);
  for(unsigned mask=0;mask<4;++mask){
   e.execution_mask[5]=bool(mask&1);Engine::FrontState fs{bool(mask&2)};
   const auto previous=s.command_writes.Target();
   e.Emit(s,fs,5,123);
   if(mask)assert(s.command_writes.Target()==s.write_position);
   else assert(s.command_writes.Target()==previous);
  }
 }
 // Every compute table entry, with and without native/trace modes, takes
 // the real frontend path and dispatches through the real ring and backend
 // store-only prefix. Unknown methods and native side effects stay producers.
 for(bool diagnostic:{false,true})for(bool native:{false,true}){
  if(diagnostic)setenv("EDEN_NATIVE_DRAW_DIAGNOSTICS","1",1);else unsetenv("EDEN_NATIVE_DRAW_DIAGNOSTICS");
  Split s;Compute e;e.Configure(native);s.system=&system;s.ring=std::make_unique<u64[]>(s.RingWords);
  for(u32 reg=0;reg<0xe00;++reg)for(unsigned route=0;route<3;++route){
   const bool plain=native&&e.native_plain_methods[reg];
   const auto producer=s.command_writes.Target(),before=s.write_position;
   const u32 value=(reg<<8)|route,values[]{value,value+1};
   if(route==0)e.FrontCallMethod(system,s,reg,value,true,nullptr);
   else if(route==1)e.FrontCallMultiMethod(system,s,reg,values,2,2,nullptr);
   else assert(e.Emit(s,reg,value));
   assert(s.command_writes.Target()==(plain?producer:s.write_position));
   auto fallback=e.fallback_calls;
   for(auto cursor=before;cursor<s.write_position;)cursor+=s.Execute(&s.ring[cursor&s.RingMask]);
   if(plain){
    assert(e.fallback_calls==fallback);
    const auto expected=route==1?value+1:value;
    assert(e.native_registers[reg]==expected);
    if(reg<e.regs.NUM_REGS)assert(e.regs.reg_array[reg]==expected);
   }else assert(e.fallback_calls>fallback);
  }
  assert(!e.native_plain_methods[reg_launch]&&!e.native_plain_methods[reg_exec_upload]&&
         !e.native_plain_methods[reg_data_upload]&&!e.native_plain_methods[0x6c3]);
  // Above native_plain_methods bounds, no annotation may drop a producer.
  for(u32 reg:{0x1000U,0xffffU}){
   e.FrontCallMethod(system,s,reg,42,true,nullptr);
   assert(s.command_writes.Target()==s.write_position);
   const u32 values[]{42};e.FrontCallMultiMethod(system,s,reg,values,1,1,nullptr);
   assert(s.command_writes.Target()==s.write_position);
  }
  const auto before=s.write_position;assert(!e.Emit(s,0xe00,42));assert(before==s.write_position);
 }
 unsetenv("EDEN_NATIVE_DRAW_DIAGNOSTICS");
 // The public state-write predicate must retain possible deferred draws.
 using Frontier=VideoCommon::CommandWriteFrontier;
 for(unsigned mask=0;mask<4;++mask){
  bool state_only=mask&1,pending=mask&2;
  assert(Frontier::StateWriteMayExecute(state_only,pending)==(!state_only||pending));
 }
 assert(trace_begins==trace_ends&&trace_begins==8);
 std::cout<<"Actual split ring PASS: first queued writer, state tail, OFF/ON ordering, "
              "mid-batch completion, unknown callbacks/multi writes, compute store-only classification, zero/short/default/clamped idle budgets and restart reset\n";
}
'''
with tempfile.TemporaryDirectory(prefix='split-frontier-') as temporary:
    folder=Path(temporary)
    (folder/'split-under-test.h').write_text(header)
    helper = folder/'video_core/split_completion.h'
    helper.parent.mkdir()
    helper.write_text((root/'Eglantine/src/video_core/split_completion.h').read_text().replace('private:', 'public:'))
    (folder/'check.cpp').write_text(code)
    subprocess.run(['clang++-18','-std=c++20','-O1','-g','-pthread',
                    '-fsanitize=thread' if args.tsan else '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer',
                    '-I'+str(folder),'-I'+str(root/'Eglantine/src'),
                    str(folder/'check.cpp'),'-o',str(folder/'check')],check=True)
    subprocess.run([str(folder/'check')],check=True,timeout=15)
