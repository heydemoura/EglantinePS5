#!/usr/bin/env python3
"""Differential descriptor-table and actual image-view lookup ownership checks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
def table_body(text):
    return text[text.index('namespace VideoCommon {'):]
actual = table_body((root/'Eglantine/src/video_core/texture_cache/descriptor_table.h').read_text())
reference = table_body((root/'tools/fixtures/descriptor-table-reference.inc').read_text()).replace('class DescriptorTable {', 'class ReferenceTable {')
source = (root/'Eglantine/src/video_core/texture_cache/texture_cache.h').read_text()
helper = source[source.index('inline bool PoisonInvalidatedViewIds()'):source.index('using Tegra::Texture::TICEntry;')]
start = source.index('template <class P>\nImageViewId TextureCache<P>::VisitImageView')
visit = source[start:source.index('template <class P>\nFramebufferId TextureCache<P>::GetFramebufferId', start)]
# Both production invalidation sites retain graphics and compute invalidation.
assert source.count('if (PoisonInvalidatedViewIds()) {') == 2
for method in ['void TextureCache<P>::InvalidateScale(', 'void TextureCache<P>::DeleteImage(']:
    begin = source.index(method)
    end = source.index('\ntemplate <class P>', begin)
    body = source[begin:end]
    assert 'channel_info.graphics_image_table.Invalidate();' in body
    assert 'channel_info.compute_image_table.Invalidate();' in body

differential = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <random>
#include <limits>
#include <cstdio>
#include <chrono>
using u64=uint64_t;using u32=uint32_t;using GPUVAddr=uint64_t;
#define DEBUG_ASSERT assert
namespace Common {template<class A,class B>auto DivCeil(A a,B b){return (a+b-1)/b;}}
namespace Tegra {struct MemoryManager {std::vector<u64> words=std::vector<u64>(2u<<20);void ReadBlockUnsafe(u64 a,void* out,size_t n)const{assert(a+n<=words.size()*8);std::memcpy(out,reinterpret_cast<const char*>(words.data())+a,n);}};}
''' + reference + actual + r'''
int main(){
 using namespace VideoCommon;Tegra::MemoryManager memory;
 DescriptorTable<u64> candidate;ReferenceTable<u64> reference;
 std::mt19937 random(0xd35c);u64 comparisons=0;
 auto sync=[&](u64 a,u32 n){assert(candidate.Synchronize(a,n)==reference.Synchronize(a,n));};
 auto read=[&](u32 i){auto a=candidate.Read(memory,i),b=reference.Read(memory,i);assert(a==b);++comparisons;};
 auto invalidate=[&]{candidate.Invalidate();reference.Invalidate();};
 constexpr std::array<u32,9> limits{0,1,63,64,65,4095,4096,524287,524288};
 for(unsigned cycle=0;cycle<27;++cycle){
  u32 limit=limits[cycle%limits.size()];u64 base=8+(cycle%2)*8*600000;
  sync(base,limit);sync(base,limit);
  for(unsigned step=0;step<20000;++step){
   u32 i=random()%(limit+1);if(step%7==0)memory.words[base/8+i]=random();
   if(step%101==0)invalidate();
   read(i);read(i); // both fresh and cached reads
  }
  for(u32 i:std::array<u32,4>{0,limit,limit/2,std::min(limit,64u)}){read(i);invalidate();read(i);}
 }
 // Simulate generation rollover with an old generation-one bit still populated.
 sync(0,65);candidate.generation=1;candidate.read_generations[0]=1;
 candidate.read_descriptors[0]=~u64{};candidate.generation=std::numeric_limits<u64>::max();
 invalidate();read(0);read(0);read(64);
 std::printf("PASS %llu descriptor comparisons, growth/shrink, rebasing, unchanged/new descriptors, repeated invalidation and generation rollover\n",(unsigned long long)comparisons);
 sync(0,524287);
 auto bench=[&](auto& table){auto begin=std::chrono::steady_clock::now();u64 sink=0;for(int i=0;i<10000;++i){table.Invalidate();sink+=table.Read(memory,64).second;}std::printf("sink %llu ",(unsigned long long)sink);return std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();};
 auto old_s=bench(reference),new_s=bench(candidate);std::printf("10k invalidate+read baseline %.6fs candidate %.6fs\n",old_s,new_s);
}
'''
views = r'''

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <cstdio>
using u64=uint64_t;using u32=uint32_t;using GPUVAddr=uint64_t;
#define DEBUG_ASSERT assert
#define LOG_INFO(...) (void)physical
#define LOG_DEBUG(...) (void)0
namespace Common {template<class A,class B>auto DivCeil(A a,B b){return (a+b-1)/b;}struct SlotId{static constexpr u32 TAGGED_VALUE=0x80000000;};}
struct TIC {u64 value;u32 Width()const{return 0;}u32 Height()const{return 0;}u64 Address()const{return value;}auto operator<=>(const TIC&)const=default;};
namespace Tegra {struct MemoryManager {std::vector<u64> words=std::vector<u64>(4096);void ReadBlockUnsafe(u64 a,void* out,size_t n)const{assert(a+n<=words.size()*8);std::memcpy(out,reinterpret_cast<const char*>(words.data())+a,n);}std::optional<u64> GpuToCpuAddress(u64 a,u64)const{return a;}};}
''' + actual + r'''
namespace VideoCommon {
using ImageViewId=u32;constexpr u32 NULL_IMAGE_VIEW_ID=0;constexpr u32 CORRUPT_ID=~u32{};
bool IsValidEntry(const Tegra::MemoryManager&,const TIC& d){return d.value!=0;}
struct Channel {DescriptorTable<TIC> graphics_image_table,compute_image_table;std::map<TIC,u32> image_views;std::map<u32,u32> image_view_ids;};
template<class P>struct TextureCache {
 Channel* channel_state;Tegra::MemoryManager* gpu_memory;std::set<u32> live;u32 next=1;
 u32 CreateImageView(const TIC&){live.insert(next);return next++;}
 void PrepareImageView(u32 id,bool,bool){assert(live.contains(id));}
 u32 VisitImageView(u32 index,bool compute);
};
''' + helper + visit + r'''
}
int main(){using namespace VideoCommon;
 Tegra::MemoryManager memory;Channel a,b;TextureCache<int> cache{&a,&memory,{},1};
 for(auto* c:{&a,&b}){c->graphics_image_table.Refresh(8,1023);c->compute_image_table.Refresh(8*1100,1023);}
 auto invalidate=[&](Channel& c){if(PoisonInvalidatedViewIds())for(auto& e:c.image_view_ids)e.second=CORRUPT_ID;c.graphics_image_table.Invalidate();c.compute_image_table.Invalidate();};
 auto erase=[&](u32 id){cache.live.erase(id);for(auto* c:{&a,&b}){std::erase_if(c->image_views,[&](auto& e){return e.second==id;});invalidate(*c);}};
 for(u32 round=0;round<1000;++round){
  const u32 index=round%1024;memory.words[1+index]=round+1;memory.words[1100+index]=round+1;
  cache.channel_state=&a;auto id=cache.VisitImageView(index,false);assert(id!=0);assert(cache.VisitImageView(index,false)==id);
  assert(cache.VisitImageView(index,true)==id);
  // Populate the other channel, then delete the view while leaving stale per-index IDs.
  cache.channel_state=&b;assert(cache.VisitImageView(index,false)!=0);auto b_id=cache.VisitImageView(index,false);
  erase(id);erase(b_id);
  // Unchanged guest descriptors must create replacement IDs in both channels and engines.
  for(auto* c:{&a,&b}){cache.channel_state=c;auto replacement=cache.VisitImageView(index,false);assert(replacement!=id&&replacement!=b_id);assert(cache.VisitImageView(index,true)==replacement);}
  // A now-invalid descriptor must replace its cached ID with the null view.
  memory.words[1+index]=0;invalidate(a);cache.channel_state=&a;assert(cache.VisitImageView(index,false)==0);assert(cache.VisitImageView(index,false)==0);
 }
 std::printf("PASS actual VisitImageView: two channels, graphics/compute, unchanged descriptor after deletion, stale ID replacement, invalid entries; poison=%d\n",PoisonInvalidatedViewIds());
}
'''

with tempfile.TemporaryDirectory(prefix='descriptor-invalidation-') as directory:
    for name, code, flags in [('differential', differential, []), ('views-ps5', views, ['-D__PROSPERO__']), ('views-desktop', views, [])]:
        executable = Path(directory)/name
        subprocess.run(['c++', '-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', *flags, '-x', 'c++', '-', '-o', str(executable)],
                       input=code, text=True, check=True)
        subprocess.run([str(executable)], check=True, timeout=30)
        if name == 'views-ps5':
            import os
            subprocess.run([str(executable)], env={**os.environ, 'EDEN_TEXTURE_CACHE_POISON_IDS': '1'}, check=True, timeout=30)
