#!/usr/bin/env python3
"""Exercise the production temporary-image method with resource-counting Vulkan/VMA APIs."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include <array>
#include <atomic>
#include <cstdio>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
using u32 = unsigned;
using VkDeviceSize = uint64_t;
using VkImage = unsigned;
using VmaPool = void*;
using VmaAllocation = void*;
using VmaAllocator = int;
constexpr int VK_SUCCESS=0, VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT=1,
 VMA_MEMORY_USAGE_UNKNOWN=0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT=1;
struct VkImageCreateInfo { unsigned usage=8; };
struct VkMemoryRequirements { uint64_t size{}; unsigned memoryTypeBits{}; };
struct VmaAllocationCreateInfo { unsigned flags{}; int usage{}; unsigned preferredFlags{}; VmaPool pool{}; };
struct VmaPoolCreateInfo { unsigned memoryTypeIndex{}; uint64_t blockSize{}; size_t minBlockCount{}, maxBlockCount{}; };
struct VmaAllocationInfo {};
struct State {
 unsigned creates{}, destroys{}, allocs{}, frees{}, pools{}, fallbacks{}, selected_type{};
 uint64_t size=4ULL<<20; unsigned bits=4;
 bool fail_create{}, fail_find{}, fail_pool{}, fail_alloc{}, fail_bind{};
} state;
struct Dispatch {
 int vkCreateImage(int,const VkImageCreateInfo*,void*,VkImage* h) const { if(state.fail_create)return -1; *h=++state.creates; return 0; }
 void vkGetImageMemoryRequirements(int,VkImage,VkMemoryRequirements* r) const { *r={state.size,state.bits}; }
 void vkDestroyImage(int,VkImage,void*) const { ++state.destroys; }
};
struct Logical { int operator*() const { return 1; } };
struct Device { Dispatch dispatch; Logical logical;
 const Dispatch& GetDispatchLoader() const { return dispatch; }
 const Logical& GetLogical() const { return logical; }
};
int vmaFindMemoryTypeIndex(int,unsigned bits,const VmaAllocationCreateInfo* r,unsigned* t) {
 assert(r->usage==VMA_MEMORY_USAGE_UNKNOWN);
 if(state.fail_find||!bits)return -1; *t=std::countr_zero(bits);return 0;
}
int vmaCreatePool(int,const VmaPoolCreateInfo* r,VmaPool* p) {
 assert(r->blockSize==(16ULL<<20)&&r->minBlockCount==0&&r->maxBlockCount==32);
 if(state.fail_pool)return -1;state.selected_type=r->memoryTypeIndex;++state.pools;*p=reinterpret_cast<void*>(uintptr_t(state.pools));return 0;
}
void vmaSetPoolName(int,void*,const char*) {}
int vmaAllocateMemoryForImage(int,VkImage,const VmaAllocationCreateInfo* r,VmaAllocation* a,VmaAllocationInfo*) {
 assert(r->pool&&r->flags==1);if(state.fail_alloc)return -1;++state.allocs;*a=reinterpret_cast<void*>(uintptr_t(state.allocs));return 0;
}
int vmaBindImageMemory(int,void*,VkImage) { return state.fail_bind ? -1 : 0; }
void vmaFreeMemory(int,void*) { ++state.frees; }
namespace Vulkan {
namespace vk {
struct Image {
 VkImage h{}; VmaAllocation allocation{};
 Image()=default;
 Image(VkImage h_,unsigned,int,int,VmaAllocation a,const Dispatch&) :h(h_),allocation(a){}
 Image(const Image&)=delete;
 Image(Image&& o):h(std::exchange(o.h,0)),allocation(std::exchange(o.allocation,nullptr)){}
 ~Image(){ if(h)++state.destroys;if(allocation)++state.frees; }
};
}
struct MemoryAllocator {
 Device device;
 int allocator=1;
 unsigned valid_memory_types=~0u;
 mutable std::array<VmaPool,32> temporary_pools{};
 mutable std::mutex temporary_pool_mutex;
 vk::Image CreateImage(const VkImageCreateInfo&) const { ++state.fallbacks;return {}; }
 vk::Image CreateTemporaryImage(const VkImageCreateInfo&) const;
};
'''
code += (root/'headless/transient_images.inc').read_text() + '\n}\n'
code += r'''
int main(){
 using Vulkan::MemoryAllocator;
 VkImageCreateInfo ci;
 unsetenv("EDEN_PS5_TRANSIENT_POOLS");{MemoryAllocator a;auto image=a.CreateTemporaryImage(ci);assert(state.fallbacks==1&&state.creates==0);}
 setenv("EDEN_PS5_TRANSIENT_POOLS","1",1);
 state={};
 {MemoryAllocator a;
  {auto one=a.CreateTemporaryImage(ci);auto two=a.CreateTemporaryImage(ci);assert(state.pools==1&&state.allocs==2&&state.frees==0&&state.selected_type==2);}
  assert(state.frees==2&&state.destroys==2);
  state.bits=8;{auto other=a.CreateTemporaryImage(ci);assert(state.pools==2&&state.selected_type==3);}
  state.size=(16ULL<<20)+1;{auto large=a.CreateTemporaryImage(ci);assert(state.fallbacks==1&&state.pools==2);}
 }
 assert(state.creates==state.destroys&&state.allocs==state.frees);
 for(unsigned failure=0;failure<6;++failure){
  state={};MemoryAllocator a;
  if(failure==0)state.fail_create=true;
  if(failure==1)state.fail_find=true;
  if(failure==2)state.fail_pool=true;
  if(failure==3)state.fail_alloc=true;
  if(failure==4)state.fail_bind=true;
  if(failure==5)a.valid_memory_types=0;
  {auto image=a.CreateTemporaryImage(ci);}
  assert(state.fallbacks==1&&state.creates==state.destroys&&state.allocs==state.frees);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='transient-images-') as temp:
    p=Path(temp);(p/'check.cpp').write_text(code)
    subprocess.run(['clang++-18','-std=c++20','-g','-O1','-pthread','-fsanitize=address,undefined',str(p/'check.cpp'),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],check=True,timeout=10)
print('Temporary image method PASS: opt-in, type constraints/reuse, 16MiB/32-block bound, large fallback and all partial-resource failure paths')
