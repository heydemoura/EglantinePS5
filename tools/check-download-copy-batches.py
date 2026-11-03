#!/usr/bin/env python3
"""Check actual bounded copy grouping against sequential staging copies."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'Eglantine/src/video_core/buffer_cache/buffer_cache.h').read_text()
start = source.index('template <size_t Capacity = 32, class Downloads, class Function>')
end = source.index('\ntemplate <class P>', start)
helper = source[start:end]
commit = source[source.index('void BufferCache<P>::CommitAsyncFlushesHigh()'):]
commit = commit[:commit.index('\ntemplate <class P>')]
assert commit.count('runtime.PreCopyBarrier();') == commit.count('runtime.PostCopyBarrier();') == 1
assert commit.index('runtime.PreCopyBarrier();') < commit.index('ForEachDownloadCopyBatch(downloads, record);') < commit.index('runtime.PostCopyBarrier();')
assert 'eden_download_copy_batch_enabled && eden_download_copy_batch_enabled()' in commit
assert 'for (const auto& [copy, id] : downloads) record(id, std::span{&copy, 1});' in commit
assert 'runtime.Finish()' not in commit
code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <utility>
#include <vector>
using u64=uint64_t;
struct Copy {u64 src_offset,dst_offset,size; bool operator==(const Copy&)const=default;};
''' + helper + r'''
template<size_t Capacity>void run(const std::vector<std::pair<Copy,unsigned>>& input){
 std::array<std::array<unsigned char,4096>,4> sources;
 for(unsigned id=0;id<4;++id)for(unsigned i=0;i<4096;++i)sources[id][i]=(i*13+id*57)%251;
 std::vector<unsigned char> expected(input.size()*96+64,0xcd),actual=expected;
 for(auto [copy,id]:input)std::memcpy(expected.data()+copy.dst_offset,sources[id].data()+copy.src_offset,copy.size);
 std::vector<std::pair<Copy,unsigned>> flattened;
 unsigned commands{};bool have_last{};unsigned last_id{};size_t last_count{};
 ForEachDownloadCopyBatch<Capacity>(input,[&](unsigned id,std::span<const Copy> copies){
  assert(!copies.empty()&&copies.size()<=Capacity);
  if(have_last&&id==last_id)assert(last_count==Capacity);
  last_id=id;last_count=copies.size();have_last=true;++commands;
  for(auto copy:copies){flattened.emplace_back(copy,id);std::memcpy(actual.data()+copy.dst_offset,sources[id].data()+copy.src_offset,copy.size);}
 });
 assert(flattened==input&&actual==expected&&commands<=input.size());
 if(Capacity==1)assert(commands==input.size());
 if(input.empty())assert(!commands);
}
int main(){
 std::mt19937 random{17531};
 for(unsigned n:{0,1,31,32,33,64,65,257,4096,100000}){
  std::vector<std::pair<Copy,unsigned>> copies;
  unsigned id{};u64 destination{};
  for(unsigned i=0;i<n;++i){
   if(random()%5==0)id=random()%4;
   u64 size=1+random()%48,offset=random()%(4096-size);
   copies.push_back({{offset,destination,size},id});destination+=(size+63)&~u64{63};
  }
  run<1>(copies);run<32>(copies);
 }
 std::vector<std::pair<Copy,unsigned>> aba{{{0,0,4},0},{{0,64,4},1},{{4,128,4},0}};run<32>(aba);
 // Repeated identical sources collapse to ceil(n/32), without crossing capacity.
 std::vector<std::pair<Copy,unsigned>> repeated(100);unsigned calls{};
 ForEachDownloadCopyBatch(repeated,[&](unsigned,std::span<const Copy> c){++calls;assert(c.size()<=32);});assert(calls==4);
}
'''
with tempfile.TemporaryDirectory(prefix='download-copy-batches-') as temp:
    path = Path(temp)
    (path / 'check.cpp').write_text(code)
    subprocess.run(['clang++-18', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined',
                    str(path / 'check.cpp'), '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True, timeout=20)
print('Download copy batches PASS: 100,000-region staging oracle, exact order, A/B/A sources, empty/capacity boundaries, existing barriers/fence retained')
