#!/usr/bin/env python3
"""Check fail-closed pool enumeration and lock-free VMA block accounting."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
code = r'''
#include <cassert>
#include <thread>
#include <vector>
#include "direct_pool_probe.h"
#include "device_allocation_probe.h"
using namespace Eden::DirectPoolProbe;
int main(){
 const std::vector<Region> regions{{10,20,12},{30,45,2},{60,80,12}};
 auto query=[&](std::int64_t cursor,Region& out){for(auto r:regions)if(r.end>cursor){out=r;return 0;}return -5;};
 auto free_gap=[&](std::int64_t a,std::int64_t b,int& status){status=0;for(auto r:regions)if(r.start<b&&a<r.end)return false;return true;};
 auto proceed=[]{return true;};
 auto r=Scan(100,16,query,free_gap,proceed);
 assert(r.complete&&r.regions==3&&r.occupied==45&&r.verified_free==55&&r.Free(100)==55);
 assert(Stable(r,Scan(100,16,query,free_gap,proceed)));
 auto changed=r;changed.fingerprint^=1;assert(!Stable(r,changed));
 auto limited=Scan(100,2,query,free_gap,proceed);assert(limited.limited&&!limited.complete&&!limited.Free(100));
 auto timed=Scan(100,16,query,free_gap,[]{return false;});assert(timed.limited&&!timed.Free(100));
 auto error_tail=[](auto,auto,int& status){status=-77;return false;};
 auto qend=[](auto,Region&){return -9;};
 auto empty=Scan(100,16,qend,free_gap,proceed);assert(!empty.complete&&!empty.Free(100)); // Hidden allocations contradict an empty view.
 auto all_free=[](auto,auto,int& status){status=0;return true;};
 auto empty_pool=Scan(100,16,qend,all_free,proceed);assert(empty_pool.complete&&empty_pool.Free(100)==100);
 auto bad_tail=Scan(100,16,qend,error_tail,proceed);assert(!bad_tail.complete&&!bad_tail.Free(100)&&bad_tail.tail_status==-77);
 auto gap_fail=Scan(100,16,query,error_tail,proceed);assert(gap_fail.unverified_gap&&!gap_fail.Free(100));
 assert(!Scan(0,16,query,all_free,proceed).Free(0));
 auto malformed=[](auto,Region& out){out={20,10,12};return 0;};
 assert(Scan(100,16,malformed,all_free,proceed).invalid);
 auto overflow=[](auto,Region& out){out={0,101,12};return 0;};assert(Scan(100,16,overflow,all_free,proceed).invalid);
 auto negative=[](auto,Region& out){out={0,20,-1};return 0;};assert(Scan(100,16,negative,all_free,proceed).invalid);
 unsigned calls=0;
 auto overlap=[&](auto,Region& out){out=++calls==1?Region{0,20,12}:Region{10,30,12};return 0;};
 assert(Scan(100,16,overlap,all_free,proceed).invalid);
 const std::vector<Region> packed{{0,40,12},{40,100,2}};
 auto qpacked=[&](auto cursor,Region& out){for(auto x:packed)if(x.end>cursor){out=x;return 0;}return -1;};
 auto used=Scan(100,16,qpacked,all_free,proceed);assert(used.complete&&used.Free(100)==0);
 Eden::DeviceAllocationProbe::Recorder recorder;
 constexpr std::uint64_t MiB=1ULL<<20;
 for(auto bytes:{16*MiB,64*MiB,128*MiB,256*MiB,512*MiB,1024*MiB,1024*MiB+1})recorder.Event(3,bytes,true);
 auto v=recorder.Read(3);assert(v.allocations==7&&v.max_allocation==1024*MiB+1&&v.max_since_probe==v.max_allocation);
 for(auto bucket:v.buckets)assert(bucket==1);
 assert(recorder.Read(3).max_since_probe==0&&recorder.Read(3).max_allocation==1024*MiB+1);
 for(auto bytes:{16*MiB,64*MiB,128*MiB,256*MiB,512*MiB,1024*MiB,1024*MiB+1})recorder.Event(3,bytes,false);
 v=recorder.Read(3);assert(!v.live_bytes&&v.allocated_bytes==v.freed_bytes&&v.frees==7&&!v.anomalies);
 recorder.Event(3,1,false);assert(recorder.Read(3).anomalies==1&&!recorder.Read(3).live_bytes);
 recorder.Event(32,4,true);assert(recorder.InvalidType()==1&&recorder.Read(32).anomalies==1);
 std::vector<std::thread> threads;
 for(unsigned i=0;i<8;++i)threads.emplace_back([&]{for(unsigned j=0;j<10000;++j){recorder.Event(5,4096,true);recorder.Event(5,4096,false);}});
 for(auto& t:threads)t.join();v=recorder.Read(5);
 assert(v.allocations==80000&&v.frees==80000&&!v.live_bytes&&!v.anomalies&&v.allocated_bytes==v.freed_bytes&&v.max_allocation==4096);
}
'''
with tempfile.TemporaryDirectory(prefix='memory-probes-') as temp:
    path = Path(temp)
    (path/'check.cpp').write_text(code)
    subprocess.run(['clang++-18','-std=c++20','-O1','-g','-pthread','-fsanitize=address,undefined',
                    '-I'+str(root/'headless'),str(path/'check.cpp'),'-o',str(path/'check')],check=True)
    subprocess.run([str(path/'check')],check=True,timeout=20)
print('Memory probes PASS: independent gap/tail verification, incomplete/unstable/limited/malformed scans, true zero/empty pools, allocation buckets/maxima, balanced lifetimes and 80,000 concurrent events')
