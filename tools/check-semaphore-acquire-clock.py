#!/usr/bin/env python3
"""Exercise the production Ampere acquire path with an observable diagnostic clock."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'Eglantine/src/video_core/engines/puller.cpp').read_text()
a=s.index('    } else if (g_nx2_ampere) {',s.index('void Puller::ProcessSemaphoreTriggerMethod'))
a=s.index('{',a)+1;end=a;depth=1
while depth:
 depth+=(s[end]=='{')-(s[end]=='}');end+=1
body=s[a:end-1]
# Only replace the time provider; all acquire checks, registrations and drains are real source.
assert body.count('std::chrono::steady_clock::now()')==1
body=body.replace('std::chrono::steady_clock::now()', 'Now()')
match_start=s.index('bool Puller::PendingAcquire::Matches(')
match_end=s.index('\nbool Puller::CanResumeAcquire',match_start)
match_source=s[match_start:match_end]
code=r'''
#include <cassert>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>
using u32=unsigned;using GPUVAddr=uint64_t;
#define LOG_ERROR(...) ((void)0)
namespace VideoCommon::Nx2Profile { enum class Counter { AcquireChecks, AcquireWaits }; void Add(Counter,unsigned) {} }
enum class GpuSemaphoreOperation : u32 { AcquireEqual=1,AcquireGequal=2,AcquireMask=3 };
unsigned clocks{},drains{},releases{},adds{},removes{},reads{};
u32 first_word{},second_word{};bool latch{},mapped=true;
std::chrono::steady_clock::time_point Now(){++clocks;return std::chrono::steady_clock::time_point{std::chrono::seconds{42}};}
void DrainBack(){++drains;}
struct Memory {std::optional<GPUVAddr> GpuToCpuAddress(GPUVAddr a){return mapped?std::optional{a}:std::nullopt;}};
struct Rasterizer {void ReleaseFences(){++releases;}};
struct DmaPusher {Memory memory_manager;Rasterizer raster;Rasterizer* rasterizer=&raster;};
u32 ReadSemaphoreWord(DmaPusher&,GPUVAddr){return ++reads==1?first_word:second_word;}
struct Nx2SemaphoreWaiters {
 static Nx2SemaphoreWaiters& Get(){static Nx2SemaphoreWaiters w;return w;}
 void Add(void*,GPUVAddr,u32,u32){++adds;}
 bool Latched(void*){return latch;}
 void Remove(void*){++removes;}
};
struct Puller {
 struct PendingAcquire {
 GPUVAddr address;u32 value,operation;std::chrono::steady_clock::time_point since;bool reported;
 bool Matches(u32 current)const;
 };
 struct {struct {GPUVAddr SemaphoreAddress(){return 0x1000;}}semaphore_address;u32 semaphore_sequence=7;}regs;
 std::optional<PendingAcquire> pending_acquire;
 void Run(DmaPusher& dma_pusher,GpuSemaphoreOperation op){
'''+body+r'''
 }
};
''' + match_source + r'''
void reset(){clocks=drains=releases=adds=removes=reads=0;latch=false;mapped=true;first_word=second_word=0;}
int main(){DmaPusher dma;
 for(auto op:{GpuSemaphoreOperation::AcquireEqual,GpuSemaphoreOperation::AcquireGequal,GpuSemaphoreOperation::AcquireMask}){
  reset();Puller p;first_word=7;p.Run(dma,op);assert(clocks==0&&drains==1&&reads==1&&releases==0&&adds==0&&!p.pending_acquire);
 }
 reset();Puller blocked;blocked.Run(dma,GpuSemaphoreOperation::AcquireEqual);
 assert(clocks==1&&drains==1&&adds==1&&reads==2&&releases==1&&blocked.pending_acquire);
 assert(blocked.pending_acquire->since.time_since_epoch()==std::chrono::seconds{42});
 reset();Puller completed;second_word=7;completed.Run(dma,GpuSemaphoreOperation::AcquireEqual);
 assert(clocks==1&&adds==1&&removes==1&&reads==2&&releases==1&&!completed.pending_acquire);
 reset();Puller latched;latch=true;latched.Run(dma,GpuSemaphoreOperation::AcquireEqual);
 assert(clocks==1&&removes==1&&reads==1&&!latched.pending_acquire);
 reset();Puller unmapped;mapped=false;unmapped.Run(dma,GpuSemaphoreOperation::AcquireEqual);assert(adds==0&&unmapped.pending_acquire);
 reset();Puller invalid;invalid.Run(dma,GpuSemaphoreOperation(99));assert(clocks==0&&drains==0&&reads==0&&releases==0);
}
'''
with tempfile.TemporaryDirectory(prefix='semaphore-clock-')as temp:
 p=Path(temp);(p/'check.cpp').write_text(code)
 subprocess.run(['clang++-18','-std=c++20','-O1','-g','-fsanitize=address,undefined',str(p/'check.cpp'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
print('Semaphore acquire clock PASS: satisfied equal/gequal/mask skip clock; blocked, released, latched, unmapped and invalid paths preserve ordering')
