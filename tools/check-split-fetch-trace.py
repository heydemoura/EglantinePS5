#!/usr/bin/env python3
"""Check actual fetch storage/draining and analyzer clipping/error handling."""
from pathlib import Path
import csv
import importlib.util
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
code=r'''
#include <cassert>
#include <thread>
#include "ordered_read_trace_store.h"
using Store=Eden::OrderedReadTrace::Store<4,4>;
int main(){
 Store s;s.Start(1,100,1000,2);
 const auto token=s.BeginFetch(1100,2,6,0,1);assert(token!=Store::NoFetch);
 assert(s.inflight==1&&s.Count()==0&&s.FetchCount()==1);
 s.Frame(101,2000);s.EndFetch(token,2200);s.Frame(102,3000);
 assert(s.Ready()&&s.fetches[0].start_frame==101&&s.fetches[0].end_frame==102);
 assert(s.fetches[0].begin_ns==1100&&s.fetches[0].end_ns==2200);
 assert(s.BeginFetch(3100,6,6,6,0)==Store::NoFetch&&s.fetch_dropped==0);
 s.Start(2,200,4000,1);
 const auto across=s.BeginFetch(4100,9,12,0,1);
 s.Frame(201,5000);assert(!s.Ready()); // Last frame cannot export an in-flight fetch.
 s.EndFetch(across,5200);assert(s.Ready());
 s.Start(3,300,6000,1);
 for(unsigned i=0;i<5;++i){
  auto index=s.BeginFetch(6100+i,0,0,0,0);
  if(i==4)assert(index==Store::NoFetch);
  else{s.EndFetch(index,6200+i);assert(s.fetches[index].mode==0);}
 }
 s.Frame(301,7000);assert(s.Ready()&&s.FetchCount()==4&&s.fetch_next==5&&s.fetch_dropped==1);
 // A CPU read and fetch share the export lifetime, but retain independent IDs/counts.
 s.Start(4,400,8000,1);
 Eden::OrderedReadTrace::Record read{};assert(s.Begin(read,8100));
 auto fetch=s.BeginFetch(8200,1,2,0,1);s.Frame(401,9000);
 assert(!s.Ready());s.End(read,{});assert(!s.Ready());s.EndFetch(fetch,9200);
 assert(s.Ready()&&s.Count()==1&&s.FetchCount()==1&&s.fetch_dropped==0);
 s.Start(5,500,10000,1);
 std::atomic<bool> began{},finish{};
 std::thread writer([&]{auto index=s.BeginFetch(10100,1,2,0,1);began.store(true);
                       while(!finish.load())std::this_thread::yield();s.EndFetch(index,11200);});
 while(!began.load())std::this_thread::yield();s.Frame(501,11000);assert(!s.Ready());
 finish.store(true);writer.join();assert(s.Ready()&&s.fetches[0].end_frame==502);
}
'''
with tempfile.TemporaryDirectory(prefix='split-fetch-trace-') as temporary:
    folder=Path(temporary)
    stub=folder/'video_core/readback_dependency_diagnostics.h'
    stub.parent.mkdir()
    stub.write_text('''#pragma once
#include <cstdint>
namespace VideoCore {struct RasterizerDownloadArea {};}
namespace VideoCommon::ReadbackDependencyDiagnostics {
struct Record {std::uint64_t trace_index{},request_ns{},start_frame{},end_frame{};};
}
''')
    (folder/'check.cpp').write_text(code)
    sanitizer='thread' if '--tsan' in sys.argv else 'address,undefined'
    subprocess.run(['clang++-18','-std=c++20','-O1','-g','-pthread','-fsanitize='+sanitizer,
                    '-I'+str(folder),'-I'+str(root/'headless'),str(folder/'check.cpp'),
                    '-o',str(folder/'check')],check=True)
    subprocess.run([str(folder/'check')],check=True,timeout=15)

    spec=importlib.util.spec_from_file_location('ordered_reads',root/'tools/analyze-ordered-reads.py')
    analyzer=importlib.util.module_from_spec(spec);spec.loader.exec_module(analyzer)
    prefix=folder/'trace'
    meta=dict(fetch_waits='1',fetch_complete='1',fetch_dropped='0',fetch_count='2',fetch_attempted='2')
    values=[dict(id=0,begin_ns=110,end_ns=250,start_frame=101,end_frame=102,target=2,queued_end=6,consumed_before=0,mode=1),
            dict(id=1,begin_ns=260,end_ns=280,start_frame=102,end_frame=102,target=9,queued_end=9,consumed_before=9,mode=0)]
    def write(rows):
        with Path(str(prefix)+'-fetch-waits.csv').open('w')as output:
            writer=csv.DictWriter(output,fieldnames=list(values[0]));writer.writeheader();writer.writerows(rows)
    write(values)
    result=analyzer.analyze_fetch_waits(prefix,meta,[100,200,300],[101,102])
    assert result['available']and result['requests']==2 and result['requests_per_frame']==1
    assert result['producer_pending_at_entry']==result['queued_tail_requests']==1
    assert abs(result['overlap_cpu_ms']-.00016)<1e-12
    frames=list(csv.DictReader(Path(str(prefix)+'-fetch-summary.csv').open()))
    assert [int(row['frame'])for row in frames]==[101,102]
    assert [int(row['overlap_ns'])for row in frames]==[90,70]
    assert analyzer.analyze_fetch_waits(prefix,{},[100,200,300])=={'available':False}
    for field,value in [('fetch_complete','0'),('fetch_dropped','1')]:
        bad=dict(meta);bad[field]=value
        try:analyzer.analyze_fetch_waits(prefix,bad,[100,200,300])
        except ValueError:pass
        else:raise AssertionError('Incomplete capture accepted')
    for field,value in [('target',7),('mode',0),('end_ns',109),('consumed_before',7)]:
        bad=[dict(row)for row in values];bad[0][field]=value;write(bad)
        try:analyzer.analyze_fetch_waits(prefix,meta,[100,200,300])
        except AssertionError:pass
        else:raise AssertionError('Invalid fetch wait accepted: '+field)
print('Fetch trace PASS: actual store lifetime/overflow/cross-frame concurrency, independent read IDs, real-frame clipping and incomplete/invalid/legacy analyzer handling')
