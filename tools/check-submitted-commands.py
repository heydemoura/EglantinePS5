#!/usr/bin/env python3
"""Exercise real bounded command payloads and the actual DMA producer synchronization method."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'Eglantine/src/video_core/dma_pusher.cpp').read_text()
start = source.index('bool DmaPusher::FlushGpuWrittenCommands(')
end = source.index('\nvoid DmaPusher::ReportUnboundSubchannel', start)
method = source[start:end]
code = r'''
#include <cassert>
#include <functional>
#include <limits>
#include <thread>
#include "video_core/nx2.h"
#include "video_core/submitted_commands.h"
namespace VideoCommon {
struct GpuSplit {
 std::function<void()> drain;
 static GpuSplit& Get(){static GpuSplit split;return split;}
 bool Active(){return bool(drain);}
 void Drain(){drain();}
 void DrainCommandReads(){drain();}
};
enum class CacheType{BufferCache};
}
namespace Tegra {
struct Memory {
 bool dirty{},unmapped{},outside{}; unsigned host{},gpu{},flushes{};
 std::optional<DAddr> GpuToCpuAddress(GPUVAddr a){if(unmapped)return {};if(outside)return DAddr{1}<<36;return a;}
 bool IsMemoryDirty(GPUVAddr,size_t,VideoCommon::CacheType){return dirty;}
 void FlushRegion(GPUVAddr,size_t,VideoCommon::CacheType){host=gpu;dirty=false;++flushes;}
};
struct DmaPusher {Memory memory_manager;bool FlushGpuWrittenCommands(GPUVAddr,size_t);};
''' + method + r'''
}
struct Header {std::size_t addr,size;};
using Snapshot=VideoCommon::SubmittedCommands<unsigned>;
int main(){
 std::vector<unsigned> memory{1,2,3,4,5,6};
 const std::array<Header,4> entries{{{0,0},{0,3},{3,0},{3,3}}};
 unsigned reads=0;
 auto read=[&](std::size_t addr,std::span<unsigned> words){++reads;std::copy_n(memory.data()+addr,words.size(),words.data());};
 auto s=Snapshot::Capture(std::span<const Header>{entries},read);assert(s&&reads==2);
 assert(s->Entry(0,0)->empty()&&s->Entry(1,3)->back()==3&&s->Entry(3,3)->front()==4);
 assert(!s->Entry(1,4)&&!s->Entry(4,0)&&!s->Entry(std::numeric_limits<size_t>::max(),1));
 memory[0]=99;assert(s->Entry(1,3)->front()==1); // Guest reuse cannot change the owned words.
 auto shared=s;s.reset();assert(Snapshot::UsedBytes()>0&&shared->Entry(1,3)->front()==1);
 std::thread release([owned=std::move(shared)]() mutable {owned.reset();});release.join();assert(!Snapshot::UsedBytes());
 const std::array<Header,1> huge{{{0,std::numeric_limits<size_t>::max()}}};
 assert(!Snapshot::Capture(std::span<const Header>{huge},read));
 const std::array<Header,1> too_big{{{0,Snapshot::MaxSnapshotBytes/sizeof(unsigned)}}};
 assert(!Snapshot::Capture(std::span<const Header>{too_big},read));
 assert(!Snapshot::Capture(std::span<const Header>{},read));
 std::vector<Header> too_many(Snapshot::MaxEntries+1);assert(!Snapshot::Capture(std::span<const Header>{too_many},read));
 auto fail=[](auto,std::span<unsigned>){throw std::bad_alloc{};};
 assert(!Snapshot::Capture(std::span<const Header>{entries},fail)&&!Snapshot::UsedBytes());
 const std::array<Header,1> large{{{0,1024*1024}}};
 std::vector<std::shared_ptr<const Snapshot>> held;
 auto fill=[](auto,std::span<unsigned> words){std::fill(words.begin(),words.end(),17);};
 for(unsigned i=0;i<15;++i){auto x=Snapshot::Capture(std::span<const Header>{large},fill);assert(x);held.push_back(x);}
 assert(Snapshot::UsedBytes()<=Snapshot::BudgetBytes);
 assert(!Snapshot::Capture(std::span<const Header>{large},fill));
 held.pop_back();assert(Snapshot::Capture(std::span<const Header>{large},fill));
 held.clear();assert(!Snapshot::UsedBytes());
 for(unsigned mask=0;mask<16;++mask){
  bool resumed=mask&1,has=mask&2,written=mask&4,dirty=mask&8;
  assert(VideoCommon::CanUseSubmittedCommands(resumed,has,written,dirty)==(mask==2));
 }
 Tegra::g_nx2_ampere.store(true);Tegra::Nx2ClearGpuWrittenPages();
 constexpr GPUVAddr address=8*4096;
 Tegra::DmaPusher dma;
 VideoCommon::GpuSplit::Get().drain=[&]{dma.memory_manager.gpu=42;dma.memory_manager.dirty=true;Tegra::Nx2MarkGpuWritten(address,4);};
 bool written=dma.FlushGpuWrittenCommands(address,4);
 assert(written&&dma.memory_manager.host==42&&!dma.memory_manager.dirty&&dma.memory_manager.flushes==1);
 assert(!VideoCommon::CanUseSubmittedCommands(false,true,written,dma.memory_manager.dirty));
 Tegra::Nx2ClearGpuWrittenPages();unsigned drains=0;
 VideoCommon::GpuSplit::Get().drain=[&]{++drains;};
 assert(!dma.FlushGpuWrittenCommands(address,4)&&drains==1&&dma.memory_manager.flushes==1);
 assert(!dma.FlushGpuWrittenCommands(address,0)&&drains==1);
 dma.memory_manager.unmapped=true;assert(dma.FlushGpuWrittenCommands(address,4));
 dma.memory_manager.unmapped=false;dma.memory_manager.outside=true;assert(dma.FlushGpuWrittenCommands(address,4));
 Tegra::g_nx2_ampere.store(false);assert(!dma.FlushGpuWrittenCommands(address,4)&&drains==3);
 VideoCommon::GpuSplit::Get().drain={};
}
'''
with tempfile.TemporaryDirectory(prefix='submitted-commands-') as temp:
    path = Path(temp)
    (path / 'check.cpp').write_text(code)
    subprocess.run(['clang++-18', '-std=c++20', '-O1', '-g', '-pthread',
                    '-fsanitize=address,undefined', '-I'+str(root/'Eglantine/src'),
                    str(path/'check.cpp'), '-o', str(path/'check')], check=True)
    subprocess.run([str(path/'check')], check=True, timeout=30)
print('Submitted commands PASS: immutable entries, bounds/overflow, bounded live budget, shared/cross-thread lifetime, allocation rollback, all selection masks, actual first-writer drain/flush and stale-snapshot rejection')
