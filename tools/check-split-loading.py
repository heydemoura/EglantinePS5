#!/usr/bin/env python3
"""Exercise the actual loading tick with an owned concurrent renderer queue."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'headless/graphics.cpp').read_text()
start = source.index('bool LoadingTick(VideoCore::RendererBase& renderer, bool idle) {')
end = source.index('\nGraphicsWindow::GraphicsWindow(', start)
method = source[start:end]
assert 'std::atomic<bool> vulkan_loading{};' in source
assert 'std::atomic<double> vulkan_loading_start{-1};' in source
assert 'std::atomic<unsigned> vulkan_loading_frames{};' in source
code = r'''
#define EDEN_PS5_VULKAN 1
#include <cassert>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>
#include "hud.h"
namespace VideoCore { struct RendererBase {}; }
namespace VideoCommon {
thread_local bool back_context{};
class GpuSplit {
public:
    bool active{};
    std::mutex mutex;
    std::condition_variable_any ready;
    bool paused{};
    std::queue<std::function<void()>> pending;
    std::jthread worker;
    GpuSplit():worker([this](std::stop_token stop){
        back_context=true;
        for(;;){
            std::function<void()> task;
            {
                std::unique_lock lock{mutex};
                if(!ready.wait(lock,stop,[&]{return !paused && !pending.empty();}))break;
                task=std::move(pending.front());pending.pop();
            }
            task();
        }
    }){}
    ~GpuSplit(){worker.request_stop();worker.join();}
    static GpuSplit& Get(){static GpuSplit instance;return instance;}
    static bool IsBackThread(){return back_context;}
    bool Active()const{return active;}
    void DeferFunction(std::function<void()> function){
        std::lock_guard lock{mutex};pending.push(std::move(function));
    }
    void Publish(){ready.notify_one();}
    void Pause(){std::lock_guard lock{mutex};paused=true;}
    void Resume(){{std::lock_guard lock{mutex};paused=false;}ready.notify_one();}
    void Drain(){std::promise<void> done;auto future=done.get_future();
        DeferFunction([&]{done.set_value();});Publish();future.get();}
};
}
namespace Eden {
std::atomic<bool> vulkan_loading{};
std::atomic<double> vulkan_loading_start{-1};
std::atomic<unsigned> vulkan_loading_frames{};
HudSnapshot vulkan_hud;
// LoadingPace's timing is covered by check-hud.py; every request is due here.
struct Pace {bool Due(double,bool){return true;}} vulkan_loading_pace;
struct Context {bool LoadingTick(bool){return false;}};
Context* current_window_context{};
double LoadingCalm(){return 0;}
std::atomic<unsigned> splash_calls{};
int sceSystemServiceHideSplashScreen(){++splash_calls;return 0;}
std::vector<unsigned> presented;
bool require_back{};
void PresentVulkanLoading(VideoCore::RendererBase&){
    assert(VideoCommon::GpuSplit::IsBackThread()==require_back);
    assert(vulkan_hud.loading>0);presented.push_back(vulkan_hud.loading);
}
''' + method + r'''
}
int main(){
    using namespace Eden;
    VideoCore::RendererBase renderer;
    auto& split=VideoCommon::GpuSplit::Get();
    // Unsplit mode remains immediate; finished loading takes the fallback.
    vulkan_loading=true;vulkan_loading_start=-1;require_back=false;
    assert(LoadingTick(renderer,true));assert(presented.size()==1);
    vulkan_loading=false;assert(!LoadingTick(renderer,true));
    presented.clear();split.active=true;require_back=true;
    // The front must leave renderer-owned HUD storage untouched while queued.
    split.Pause();vulkan_loading=true;vulkan_loading_start=-1;vulkan_loading_frames=0;
    vulkan_hud={};vulkan_hud.width=42;
    for(unsigned i=0;i<1000;++i)assert(LoadingTick(renderer,i%2));
    assert(vulkan_hud.width==42 && presented.empty());
    split.Resume();split.Drain();assert(presented.size()==1000);
    assert(vulkan_loading_frames.load()==1000);
    presented.clear();
    // Guest presentation ends loading before queued requests execute. None of
    // them may overwrite the guest HUD or present a late loading frame.
    split.Pause();vulkan_loading=true;vulkan_loading_start=-1;vulkan_loading_frames=0;
    split.DeferFunction([]{
        (void)vulkan_loading_start.load();(void)vulkan_loading_frames.load();
        vulkan_loading=false;vulkan_hud={};vulkan_hud.width=99;
    });
    for(unsigned i=0;i<1000;++i)assert(LoadingTick(renderer,true));
    split.Resume();split.Drain();assert(presented.empty() && vulkan_hud.width==99);
    // Concurrent front requests and the first guest composite: only atomics
    // cross owners; all HUD reads/writes stay serialized on the renderer queue.
    for(unsigned round=0;round<30;++round){
        vulkan_loading=true;vulkan_loading_start=-1;vulkan_loading_frames=0;
        for(unsigned i=0;i<100;++i){
            LoadingTick(renderer,true);
            if(i==20){split.DeferFunction([]{vulkan_loading=false;vulkan_hud={};});split.Publish();}
        }
        split.Drain();
    }
    // Calling on the back thread executes directly instead of requeueing.
    presented.clear();
    split.DeferFunction([&]{vulkan_loading=true;assert(LoadingTick(renderer,true));});
    split.Publish();split.Drain();assert(presented.size()==1);
}
'''
with tempfile.TemporaryDirectory(prefix='split-loading-') as name:
    executable = Path(name)/'check'
    sanitizer = os.environ.get('EDEN_LOADING_SANITIZER', 'address,undefined')
    layout = ['-fno-pie', '-no-pie'] if sanitizer == 'thread' else []
    subprocess.run(['c++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize='+sanitizer, *layout, '-I'+str(root/'headless'),
                    '-x', 'c++', '-', '-o', str(executable)], input=code, text=True, check=True)
    subprocess.run([str(executable)], check=True, timeout=30)
print('Actual loading tick PASS: immediate/deferred owner, queued HUD snapshots, late-frame cancellation, concurrent guest transition, direct back-thread call')
