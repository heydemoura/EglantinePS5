#!/usr/bin/env python3
"""Exercise the actual disk-load flow and invalid-cache loaders with stub pipelines."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'Eglantine/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp').read_text()
load = source[source.index('void PipelineCache::LoadDiskResources('):
              source.index('\nvoid PipelineCache::QueueVulkanPipelineCacheFlush()')]
driver = source[source.index('vk::PipelineCache PipelineCache::LoadVulkanPipelineCache('):
                source.rindex('\n} // namespace Vulkan')]
destructor = source[source.index('PipelineCache::~PipelineCache()'):
                    source.index('\nGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline()')]
env = (root / 'Eglantine/src/video_core/shader_environment.cpp').read_text()
loader = env[env.index('void LoadPipelines('):env.rindex('\n} // namespace VideoCommon')]
code = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <vector>
#define LOG_INFO(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
using u32=std::uint32_t; using u64=std::uint64_t;
unsigned mode{}, serialized{}, driver_creates{}, graphics_built{}, compute_built{};
unsigned eden_pipeline_preload_mode() __attribute__((weak));
unsigned eden_pipeline_preload_mode(){return mode;}
std::filesystem::path base;
constexpr u32 CACHE_VERSION=25;
constexpr std::array<char,8> MAGIC_NUMBER{'y','u','z','u','c','a','c','h'};
constexpr std::array<char,8> VULKAN_CACHE_MAGIC_NUMBER{'y','u','z','u','v','k','c','h'};
namespace Common {
template<typename R,typename... A> using UniqueFunction=std::function<R(A...)>;
namespace FS {
enum class EdenPath{ShaderDir};
auto GetEdenPath(EdenPath){return base;}
bool CreateDir(const std::filesystem::path& p){std::filesystem::create_directories(p);return true;}
bool RemoveFile(const std::filesystem::path& p){return std::filesystem::remove(p);}
auto PathToUTF8String(const std::filesystem::path& p){return p.string();}
}}
namespace fmt {auto format(const char*,u64 id){char s[20];std::snprintf(s,sizeof(s),"%016llx",(unsigned long long)id);return std::string{s};}}
namespace Shader {enum class Stage:u32{Compute,Vertex,Task,Mesh};struct Environment{virtual ~Environment()=default;};}
namespace VideoCore {enum class LoadCallbackStage{Build};using DiskResourceLoadCallback=std::function<void(LoadCallbackStage,size_t,size_t)>;}
namespace VideoCommon {
struct FileEnvironment:Shader::Environment {
 Shader::Stage stage{};
 void Deserialize(std::ifstream& f){std::array<u64,5> sizes{};f.read(reinterpret_cast<char*>(sizes.data()),sizeof(sizes));f.read(reinterpret_cast<char*>(&stage),sizeof(stage));}
 auto ShaderStage()const{return stage;}
};
''' + loader + r'''
}
namespace boost::container {template<typename T,size_t N>using static_vector=std::vector<T>;}
template<typename T>auto MakeSpan(T& x){return std::span{x};}
struct VkPipelineCacheCreateInfo{unsigned sType;const void* pNext;unsigned flags;size_t initialDataSize;const void* pInitialData;};
constexpr unsigned VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO=1;
namespace vk {struct PipelineCache {std::vector<char> data;void Read(size_t* n,void*)const{*n=data.size();}};}
struct Device {
 bool IsKhrPipelineExecutablePropertiesEnabled()const{return false;}
 const Device& GetLogical()const{return *this;}
 vk::PipelineCache CreatePipelineCache(const VkPipelineCacheCreateInfo& ci)const {
  ++driver_creates;vk::PipelineCache cache;
  if(ci.initialDataSize){const auto* p=static_cast<const char*>(ci.pInitialData);cache.data.assign(p,p+ci.initialDataSize);}
  return cache;
 }
};
struct PipelineStatistics{explicit PipelineStatistics(const Device&){};void Report(){}};
struct ShaderPools{};
struct Worker {
 unsigned waits{};std::vector<std::function<void()>> pending;
 template<typename F>void QueueWork(F f){pending.push_back(std::move(f));}
 void WaitForRequests(std::stop_token={}){++waits;for(auto& f:pending)f();pending.clear();}
};
struct State {
 u32 extended_dynamic_state{},extended_dynamic_state_2{},extended_dynamic_state_2_logic_op{},
 extended_dynamic_state_3_blend{},extended_dynamic_state_3_enables{},color_write_enable_dynamic{},
 dynamic_vertex_input{},provoking_vertex_last{},xfb_enabled{};
};
struct ComputePipelineCacheKey{u32 key;};
struct GraphicsPipelineCacheKey{State state;};
struct Dynamic {
 bool has_extended_dynamic_state{},has_extended_dynamic_state_2{},has_extended_dynamic_state_2_logic_op{},
 has_extended_dynamic_state_3_blend{},has_extended_dynamic_state_3_enables{},has_color_write_enable{},
 has_dynamic_vertex_input{},has_provoking_vertex_last_mode{},has_provoking_vertex_tf_preserve{};
};
struct Pipelines {
 unsigned count{};
 template<typename Key>void emplace(const Key&,std::unique_ptr<int>){++count;}
};
namespace Vulkan {
using VideoCommon::FileEnvironment;
struct PipelineCache {
 Device device;bool use_vulkan_pipeline_cache=true;
 std::filesystem::path pipeline_cache_filename,vulkan_pipeline_cache_filename;
 vk::PipelineCache vulkan_pipeline_cache;Worker workers,serialization_thread;
 Dynamic dynamic_features;Pipelines graphics_cache,compute_cache;
 std::atomic<size_t> last_cache_size{};std::chrono::steady_clock::time_point last_flush;
 ~PipelineCache();
 void LoadDiskResources(u64,std::stop_token,const VideoCore::DiskResourceLoadCallback&);
 vk::PipelineCache LoadVulkanPipelineCache(const std::filesystem::path&,u32,bool=true);
 void SerializeVulkanPipelineCache(const std::filesystem::path&,const vk::PipelineCache&,u32){++serialized;}
 template<typename... T>auto CreateGraphicsPipeline(T&&...){++graphics_built;return std::make_unique<int>(1);}
 template<typename... T>auto CreateComputePipeline(T&&...){++compute_built;return std::make_unique<int>(2);}
};
''' + destructor + load + driver + r'''
}
void Write(const std::filesystem::path& path,const std::string& bytes){std::ofstream f(path,std::ios::binary);f.write(bytes.data(),bytes.size());}
std::string Read(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>{f},{}};}
template<typename T>void Append(std::string& s,const T& t){s.append(reinterpret_cast<const char*>(&t),sizeof(t));}
int main(int argc,char** argv){
 assert(argc==2);base=argv[1];auto dir=base/"0000000000000001";std::filesystem::create_directories(dir);
 auto cpu=dir/"vulkan.bin",gpu=dir/"vulkan_pipelines.bin";
 std::string keys(MAGIC_NUMBER.data(),MAGIC_NUMBER.size());Append(keys,CACHE_VERSION);
 u32 one=1;auto environment=[&](Shader::Stage stage){Append(keys,one);for(unsigned i=0;i<5;++i)Append(keys,u64{0});Append(keys,stage);};
 environment(Shader::Stage::Compute);Append(keys,ComputePipelineCacheKey{42});
 environment(Shader::Stage::Vertex);Append(keys,GraphicsPipelineCacheKey{});
 // An otherwise-valid task pipeline must still be rejected by the existing filter.
 environment(Shader::Stage::Task);Append(keys,GraphicsPipelineCacheKey{});
 std::string binary(VULKAN_CACHE_MAGIC_NUMBER.data(),VULKAN_CACHE_MAGIC_NUMBER.size());Append(binary,CACHE_VERSION);binary+="ABC";
 for(unsigned value:{1u,2u,1u,2u,0u,3u}) {
  mode=value;Write(cpu,keys);Write(gpu,binary);serialized=graphics_built=compute_built=driver_creates=0;
  unsigned progress=0;bool empty_progress=false;
  {
   Vulkan::PipelineCache cache;
   cache.LoadDiskResources(0,{},[&](auto,auto,auto){assert(false);});assert(!driver_creates);
   cache.LoadDiskResources(1,{},[&](auto,size_t built,size_t total){++progress;if(!built&&!total)empty_progress=true;});
   assert(driver_creates==1&&cache.vulkan_pipeline_cache.data==std::vector<char>({'A','B','C'}));
   bool demand=value==2,readonly=value==1||demand;
   assert(graphics_built==!demand&&compute_built==!demand);
   assert(cache.graphics_cache.count==!demand&&cache.compute_cache.count==!demand);
   assert(cache.pipeline_cache_filename.empty()==readonly&&cache.vulkan_pipeline_cache_filename.empty()==readonly);
   assert(empty_progress==demand&&progress>=1);
   assert(cache.workers.pending.empty()&&cache.workers.waits==!demand);
   assert(serialized==(readonly?0u:1u));
  }
  assert(serialized==((value==1||value==2)?0u:2u));
  assert(Read(cpu)==keys&&Read(gpu)==binary);
 }
 // Invalid input must be ignored without deleting either comparison cache.
 for(const std::string& bad:{std::string{"cut"},std::string(20,'!')}) {
  for(bool allow:{false,true}) {
   Write(cpu,bad);Write(gpu,bad);
   VideoCommon::LoadPipelines({},cpu,CACHE_VERSION,[](auto&,auto){assert(false);},[](auto&,auto){assert(false);},allow);
   Vulkan::PipelineCache cache;auto driver_cache=cache.LoadVulkanPipelineCache(gpu,CACHE_VERSION,allow);
   assert(driver_cache.data.empty());
   assert(std::filesystem::exists(cpu)==!allow&&std::filesystem::exists(gpu)==!allow);
   if(!allow)assert(Read(cpu)==bad&&Read(gpu)==bad);
  }
 }
 // A partial environment after valid entries must not truncate the read-only file.
 std::string partial=keys;Append(partial,one);partial+="x";Write(cpu,partial);
 VideoCommon::LoadPipelines({},cpu,CACHE_VERSION,[](auto& f,auto){ComputePipelineCacheKey k;f.read(reinterpret_cast<char*>(&k),sizeof(k));},[](auto& f,auto){GraphicsPipelineCacheKey k;f.read(reinterpret_cast<char*>(&k),sizeof(k));},false);
 assert(Read(cpu)==partial);
 VideoCommon::LoadPipelines({},cpu,CACHE_VERSION,[](auto& f,auto){ComputePipelineCacheKey k;f.read(reinterpret_cast<char*>(&k),sizeof(k));},[](auto& f,auto){GraphicsPipelineCacheKey k;f.read(reinterpret_cast<char*>(&k),sizeof(k));},true);
#ifdef PS5_RECOVERY_READER
 assert(Read(cpu)==keys); // Normal PS5 repair still preserves the valid prefix.
#else
 assert(!std::filesystem::exists(cpu)); // The upstream default still removes it.
#endif
 // Missing driver cache retains a usable empty handle; CPU-only prewarm remains valid.
 mode=1;Write(cpu,keys);Vulkan::PipelineCache cache;cache.use_vulkan_pipeline_cache=false;
 auto before=driver_creates;cache.LoadDiskResources(1,{},[](auto,auto,auto){});
 assert(driver_creates==before&&cache.graphics_cache.count==1&&cache.pipeline_cache_filename.empty());
 auto empty=cache.LoadVulkanPipelineCache(gpu,CACHE_VERSION,false);assert(empty.data.empty());
}
'''
with tempfile.TemporaryDirectory(prefix='pipeline-preload-') as temp:
    path = Path(temp)
    port_reader = (root / 'headless/shader_cache_reader.inc').read_text()
    for name, actual in [('core', loader), ('ps5', port_reader)]:
        (path / 'check.cpp').write_text(code.replace(loader, actual))
        subprocess.run(['clang++-18', '-std=c++20', '-O1', '-g', '-pthread',
                        '-DPS5_RECOVERY_READER=1' if name == 'ps5' else '-UPS5_RECOVERY_READER',
                        '-fsanitize=address,undefined', str(path / 'check.cpp'),
                        '-o', str(path / 'check')], check=True)
        subprocess.run([str(path / 'check'), str(path / name)], check=True, timeout=20)
print('Pipeline preload PASS: actual disk-load flow, retained driver binary, demand/prewarm, '
      'read-only paths/destructor, invalid/truncated/missing files and unchanged normal mode')
