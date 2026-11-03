#include "video_core/mapped_readback_policy.h"
#include <cassert>
#include <thread>
#include <iostream>
using Policy = VideoCommon::MappedReadbackPolicy;
int main() {
    Policy off; assert(!off.Prefer(4096,65536));
    Policy p{true}; assert(!p.Prefer(~0ULL-16,65536));
    assert(!p.Prefer(4096,0)); assert(!p.Prefer(4096,Policy::MaxBuffer+1));
    assert(p.Prefer(65536,65536));
    assert(p.Wants(65536,65536,false)); assert(p.Wants(0,131072,false));
    assert(!p.Wants(65536,65536,true)); assert(!p.Wants(65537,65535,false));
    auto lease=p.Reserve(65536,65536,false);assert(lease&&p.Used()==65536);
    auto retained=lease;lease.reset();assert(p.Used()==65536);
    retained.reset();assert(p.Used()==0);
    assert(p.Prefer(0,Policy::MaxBuffer));
    std::vector<std::shared_ptr<Policy::Lease>> full;
    for(unsigned i=0;i<Policy::Budget/Policy::MaxBuffer;++i){auto x=p.Reserve(0,Policy::MaxBuffer,false);assert(x);full.push_back(x);}
    assert(p.Used()==Policy::Budget);assert(!p.Reserve(0,65536,false));
    std::thread release([leases=std::move(full)]()mutable{leases.clear();});release.join();assert(p.Used()==0);
    std::shared_ptr<Policy::Lease> surviving;
    {Policy temporary{true};assert(temporary.Prefer(0,65536));surviving=temporary.Reserve(0,65536,false);}
    surviving.reset(); // Budget state outlives its runtime owner.
    Policy large{true};
    constexpr unsigned hot_arena = 11468800;
    assert(large.Prefer(0x10000000,hot_arena));
    auto hot=large.Reserve(0x10000000,hot_arena,false);assert(hot&&large.Used()==hot_arena);
    assert(!large.Reserve(0x10000000,hot_arena,true));hot.reset();assert(large.Used()==0);
    std::cout<<"PASS bounded allocation policy, containment, sparse/overflow rejection, shared lifetime and cross-thread release\n";
}
