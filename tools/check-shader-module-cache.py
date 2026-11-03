#!/usr/bin/env python3
"""Exercise the actual shader-reuse cache and graphics build scheduling with owned mock modules."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
build_cache = Path((root/'.local/headless-cache').read_text().strip())
config = (build_cache/'native-local/CMakeCache.txt').read_text()
boost = Path(next(line.split('=', 1)[1] for line in config.splitlines()
                  if line.startswith('Boost_SOURCE_DIR:')))
boost_includes = ['-I'+str(p) for p in sorted(boost.glob('libs/*/include'))]
assert boost_includes, 'Prepare the pinned Boost dependency first'
source = root/'Eglantine/src/video_core/renderer_vulkan'
header = (source/'vk_shader_reuse.h').read_text()
implementation = (source/'vk_shader_reuse.cpp').read_text()
cache = header[header.index('class ShaderReuseCache {'):header.rindex('\n} // namespace Vulkan')]
methods = implementation[implementation.index('ShaderReuseCache::Lookup ShaderReuseCache::Find'):
                         implementation.rindex('\n} // namespace Vulkan')]
start = header.index('    [[nodiscard]] size_t MemoryCost()')
cost = header[start:header.index('\n};', start)]
graphics = (source/'vk_graphics_pipeline.cpp').read_text()
start = graphics.index('    // Select this while the constructor')
end = graphics.index('\n}\n\nvoid GraphicsPipeline::AddTransition', start)
build = graphics[start:end]
code = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
#include <boost/container/small_vector.hpp>
#include "common/scope_exit.h"
using u64 = std::uint64_t;
namespace Common { template<class K,class V,class H> using unordered_map = std::unordered_map<K,V,H>; }
namespace Shader { struct Environment { int value; }; }
struct ShaderTranslationKey { u64 id{}; bool operator==(const ShaderTranslationKey&) const = default; };
struct ShaderTranslationKeyHash { size_t operator()(const ShaderTranslationKey& key) const { return key.id; } };
std::atomic<int> live_modules{}, destroyed_modules{};
struct Module {
    std::function<void()> on_destroy;
    Module() { ++live_modules; }
    ~Module() { if(on_destroy) on_destroy(); --live_modules; ++destroyed_modules; }
};
struct ReusableShaders {
    std::shared_ptr<Module> modules = std::make_shared<Module>();
    size_t spirv_bytes{};
    std::vector<int> reads;
    bool FitEnvironments(std::span<Shader::Environment* const> envs) const {
        return envs.size()==1 && envs[0]->value==reads[0];
    }
''' + cost + '\n};\n' + cache + '\n' + methods + r'''
std::shared_ptr<ReusableShaders> Make(size_t bytes,int variant=0) {
    auto result=std::make_shared<ReusableShaders>();result->spirv_bytes=bytes;result->reads={variant};return result;
}
ShaderReuseCache::Lookup Find(ShaderReuseCache& cache,u64 key,int variant=0) {
    Shader::Environment env{variant};std::array<Shader::Environment*,1> envs{&env};return cache.Find({key},envs);
}
// The real constructor's scheduling tail, including its scope guard and both completion paths.
#define LOG_CRITICAL(...) ((void)0)
namespace vk { struct Exception:std::runtime_error { Exception():std::runtime_error("mock build failure"){} }; }
using VkRenderPass=int;
int MakeRenderPassKey(int,int) {return 0;}
struct Notify {int completed{};void MarkShaderComplete(){++completed;}};
struct Statistics {void Collect(int,int){}};
struct RenderPassCache {int Get(int){return 0;}};
struct Worker {
    bool synchronous{};
    std::function<void()> work;
    void QueueWork(std::function<void()> f){if(synchronous)f();else work=std::move(f);}
    void Run(){assert(work);auto f=std::move(work);f();}
};
int ConfigureFunc(const Module&,int){assert(live_modules>0);return 7;}
struct Pipeline {
    std::shared_ptr<Module> spv_modules;
    int stage_infos{},configure_func{},device{};
    struct {int state{};} key;
    std::optional<int> pipeline;
    std::mutex build_mutex;
    std::condition_variable build_condvar;
    bool is_built{},fail{};
    void Validate(){assert(spv_modules);}
    void MakePipeline(int){assert(spv_modules);if(fail)throw vk::Exception{};pipeline=1;}
    Pipeline(std::shared_ptr<Module> module,Worker* worker_thread,Notify* shader_notify,
             RenderPassCache& render_pass_cache,Statistics* pipeline_statistics,bool should_fail=false)
        :spv_modules{std::move(module)},fail{should_fail} {
''' + build + r'''
    }
};
int main() {
    const size_t unit=sizeof(ReusableShaders)+4;
    {
        ShaderReuseCache cache;
        for(int i=0;i<6;++i)cache.Add({1},Make(4,i));
        assert(cache.GetStats().entries==1 && cache.GetStats().bytes==4*unit);
        assert(Find(cache,1,3).shaders && Find(cache,1,4).rejected);
        cache.Add({1},Make(4,0));assert(cache.GetStats().bytes==4*unit);
        cache.SetMemoryBudget(0);assert(cache.GetStats().bytes==0 && live_modules==0);
    }
    {
        ShaderReuseCache cache;cache.SetMemoryBudget(2*unit);
        cache.Add({1},Make(4));cache.Add({2},Make(4));
        assert(Find(cache,1).shaders);cache.Add({3},Make(4));
        assert(cache.Contains({1}) && !cache.Contains({2}) && cache.Contains({3}));
        assert(cache.GetStats().bytes==2*unit && cache.GetStats().evictions==1);
        auto selected=Find(cache,1).shaders;
        auto module=selected->modules;
        std::weak_ptr<const ReusableShaders> weak=selected;
        module->on_destroy=[&cache]{assert(cache.GetStats().bytes==0);};
        selected.reset();cache.SetMemoryBudget(0);
        assert(weak.expired() && live_modules==1); // A compiling pipeline still owns the module.
        module.reset();assert(live_modules==0); // Destruction can re-enter cache stats without its mutex held.
        auto oversized=Make(4);cache.Add({4},oversized);assert(!cache.Contains({4}));
    }
    assert(live_modules==0);
    {
        ShaderReuseCache cache;cache.SetMemoryBudget(unit);
        int callbacks=0;
        auto owned=Make(4);
        owned->modules->on_destroy=[&cache,&callbacks]{
            assert(cache.GetStats().bytes==0);++callbacks;
        };
        cache.Add({1},owned);owned.reset();
        cache.SetMemoryBudget(0); // The cache is the last owner: destructor must run after unlocking.
        assert(callbacks==1 && live_modules==0);
    }
    {
        ShaderReuseCache cache;cache.SetMemoryBudget(2*unit);
        cache.Add({1},Make(4,0));cache.Add({1},Make(4,1));cache.Add({1},Make(4,2));
        assert(cache.GetStats().bytes==unit && cache.GetStats().entries==1);
        assert(Find(cache,1,0).rejected && Find(cache,1,2).shaders);
        cache.SetMemoryBudget(unit-1);assert(cache.GetStats().entries==0);
    }
    {
        ShaderReuseCache cache;
        auto huge=Make(std::numeric_limits<size_t>::max());
        assert(huge->MemoryCost()==std::numeric_limits<size_t>::max());
        cache.Add({1},huge);assert(cache.GetStats().bytes==std::numeric_limits<size_t>::max());
        cache.Add({2},Make(4));assert(!cache.Contains({1}) && cache.GetStats().bytes==unit);
    }
    {
        ShaderReuseCache cache;cache.SetMemoryBudget(9*unit);
        std::vector<std::thread> threads;
        for(unsigned t=0;t<6;++t)threads.emplace_back([&,t]{
            for(unsigned i=0;i<1500;++i){u64 key=(i*7+t)%29;cache.Add({key},Make(4,i%5));
                (void)Find(cache,key,i%5);(void)cache.Contains({key});
                auto stats=cache.GetStats();assert(stats.bytes<=stats.budget);
                if(i%127==0)cache.SetMemoryBudget((i%2?7:9)*unit);
            }
        });
        for(auto& thread:threads)thread.join();cache.SetMemoryBudget(0);assert(live_modules==0);
    }
    for(bool synchronous:{false,true})for(bool fail:{false,true}) {
        ShaderReuseCache cache;cache.SetMemoryBudget(unit);cache.Add({1},Make(4));
        auto module=Find(cache,1).shaders->modules;
        std::weak_ptr<Module> weak=module;
        Worker worker;worker.synchronous=synchronous;Notify notify;RenderPassCache render;Statistics stats;
        Pipeline pipeline(module,&worker,&notify,render,&stats,fail);module.reset();cache.SetMemoryBudget(0);
        assert(pipeline.configure_func==7);
        if(!synchronous){assert(!weak.expired());worker.Run();}
        assert(weak.expired() && pipeline.is_built && notify.completed==1);
        assert(bool(pipeline.pipeline)==!fail);
    }
    assert(live_modules==0 && destroyed_modules>0);
}
'''
with tempfile.TemporaryDirectory(prefix='shader-module-cache-') as name:
    exe = Path(name)/'check'
    sanitizer = os.environ.get('EDEN_CACHE_SANITIZER', 'address,undefined')
    layout = ['-fno-pie', '-no-pie'] if sanitizer == 'thread' else []
    result = subprocess.run(['c++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize='+sanitizer, *layout, '-I'+str(root/'Eglantine/src'), *boost_includes,
                    '-x', 'c++', '-',
                    '-o', str(exe)], input=code, text=True)
    if result.returncode:
        raise SystemExit(result.returncode)
    subprocess.run([str(exe)], check=True, timeout=30)
print('Actual shader cache/build scheduling PASS: budget/LRU, variants, overflow, concurrent access, eviction ownership, destructor reentry, immediate/deferred compilation and failure cleanup')
