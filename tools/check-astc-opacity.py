#!/usr/bin/env python3
"""Compare actual ASTC opacity code with its frozen pre-optimization parser."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'Eglantine/src/video_core/textures/astc.cpp').read_text()
def between(begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]
code = r'''
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <cstdio>
#include <chrono>
#include <random>
#include <vector>
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;
'''
code += between('class InputBitStream {', 'class OutputBitStream {')
code += between('enum class IntegerEncoding', 'namespace Tegra::Texture::ASTC {')
code += between('struct TexelWeightParams {', '// Replicates low num_bits')
code += (root/'tools/fixtures/astc-opacity-reference.inc').read_text()
code += between('bool IsOpaque(', '\n} // namespace Tegra::Texture::ASTC')
code += r'''
int main(){
 std::mt19937_64 rng(0x6a737463);std::array<u8,64> data{};u64 count=0;
 auto check=[&](size_t n){auto v=std::span<const u8>(data).first(n);if(Reference(v)!=IsOpaque(v)){std::fprintf(stderr,"mismatch case %llu\n",(unsigned long long)count);std::abort();}++count;};
 // Exhaust every mode/partition/endpoint header; vary all remaining payload bits.
 for(u32 h=0;h<(1u<<17);++h)for(u32 tail=0;tail<4;++tail){
  for(auto& x:data)x=static_cast<u8>(rng());
  data[0]=h;data[1]=h>>8;data[2]=(data[2]&0xfe)|(h>>16);check(16);
 }
 for(u32 i=0;i<200000;++i){for(auto& x:data)x=static_cast<u8>(rng());check(i%65);}
 std::printf("PASS %llu differential cases\n",(unsigned long long)count);
 // Fully scan one MiB of opaque single-partition blocks, rather than early-out.
 std::vector<u8> opaque(1u<<20); for(size_t i=0;i<opaque.size();i+=16)opaque[i]=0x42;
 volatile u64 sink=0;
 auto bench=[&](auto fn){auto begin=std::chrono::steady_clock::now();for(int i=0;i<100;++i)sink=sink+fn(opaque);return std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();};
 auto old_s=bench(Reference);auto new_s=bench(IsOpaque);std::printf("100 MiB scan reference %.6fs candidate %.6fs ratio %.3f sink %llu\n",old_s,new_s,old_s/new_s,(unsigned long long)sink);
}
'''
with tempfile.TemporaryDirectory(prefix='astc-opacity-') as directory:
    executable = Path(directory)/'check'
    subprocess.run(['c++', '-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-x', 'c++', '-', '-o', str(executable)],
                   input=code, text=True, check=True)
    subprocess.run([str(executable)], check=True, timeout=30)
