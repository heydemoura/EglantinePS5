#!/usr/bin/env python3
"""Check generated GPU publication, ordering and cancellation without a consumer."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
cache=Path((root/'.local/headless-cache').read_text().strip())
worker=(cache/'native-local/headless/gpu_thread.cpp').read_text()
start=worker.index('u64 ThreadManager::PushCommand(')
body=worker[start:worker.index('\n} // namespace',start)]
# The port replaces the core's blocking push with one that a stop releases (headless/CMakeLists.txt).
assert 'state.queue.EmplaceWait(' not in body and 'state.queue.TryEmplace(' in body
signal_start=worker.index('void SignalCommand(')
signal_body=worker[signal_start:worker.index('\n} // Anonymous namespace',signal_start)]
main=(root/'headless/main.cpp').read_text()
assert 'system.Pause();' not in main
code=r'''
#include <array>
#include <variant>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stop_token>
#include <thread>
#include "common/bounded_threadsafe_queue.h"
using u64=unsigned long long;
using CommandData=std::variant<int>;
struct CommandDataContainer {
 CommandDataContainer()=default;
 explicit CommandDataContainer(CommandData&& data_,u64 fence_,bool block_):data{std::move(data_)},fence{fence_},block{block_} {}
 CommandData data;u64 fence{};bool block{};
};
// The generated gpu_thread.h: the core's queue at the port's eight entries, and its fence state.
struct SynchState {
 using CommandQueue=Common::SPSCQueue<CommandDataContainer,8>;
 std::mutex write_lock;CommandQueue queue;u64 last_fence{};std::atomic<u64> signaled_fence{};
 std::mutex fence_mutex;std::condition_variable_any fence_cv;std::condition_variable_any cv;
};
struct ThreadManager {
 SynchState state;
 std::stop_source stop_source;
 u64 PushCommand(CommandData&&,bool,bool);
 // The worker file defines these after PushCommand; here there is no worker to end.
 void NotifyShutdown();
 void Stop() { stop_source.request_stop(); }
};
namespace VideoCommon { struct GpuSplit { static GpuSplit& Get() { static GpuSplit split; return split; } void Stop() {} }; }
// headless/CMakeLists.txt times a push that found the queue full (gpu_queue_full), so the body
// below carries Eden's Timer in development builds. This standalone TU does not include
// headless/performance.h, so the Timer is stubbed here.
namespace Eden::Performance {
struct Totals { std::atomic<unsigned long long> calls{}, nanoseconds{}, requested_bytes{}; };
inline Totals gpu_queue_full;
class Timer {
public:
    explicit Timer(Totals&, unsigned long long = 0) {}
};
} // namespace Eden::Performance
'''+signal_body+'\n'+body+r'''
int main() {
 ThreadManager manager;
 // Eight commands fit without a consumer, in order.
 for(int i=0;i<8;++i) assert(manager.PushCommand(int{i},false,true)==u64(i+1));
 // A ninth waits for room; the stop of a worker that will never take it releases the producer.
 auto full=std::async(std::launch::async,[&]{return manager.PushCommand(8,false,true);});
 assert(full.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
 manager.stop_source.request_stop();
 assert(full.wait_for(std::chrono::seconds(1))==std::future_status::ready);
 full.get();
 CommandDataContainer item;
 for(int i=0;i<8;++i) {
   assert(manager.state.queue.TryPop(item));
   assert(std::get<int>(item.data)==i && item.fence==u64(i+1));
 }
 assert(!manager.state.queue.TryPop(item));
 // After the stop nothing more is queued.
 manager.PushCommand(9,false,true);
 assert(!manager.state.queue.TryPop(item));
 // A synchronous push waiting for its fence is released by the stop too.
 ThreadManager synchronous;
 auto fence_wait=std::async(std::launch::async,[&]{return synchronous.PushCommand(5,true,false);});
 assert(fence_wait.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
 synchronous.stop_source.request_stop();
 assert(fence_wait.wait_for(std::chrono::seconds(1))==std::future_status::ready);
 fence_wait.get();
 // A blocking push returns once its own fence is signalled, not before.
 ThreadManager completed;
 auto completing=std::async(std::launch::async,[&]{return completed.PushCommand(6,true,true);});
 CommandDataContainer completion_item;
 while(!completed.state.queue.TryPop(completion_item)) std::this_thread::yield();
 assert(completing.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
 SignalCommand(completed.state,completion_item.fence,completion_item.block);
 assert(completing.get()==1);
 // Several blocking producers, released in fence order by one consumer.
 ThreadManager ordered;
 std::array<std::future<u64>,8> waits;
 for(auto& wait:waits) wait=std::async(std::launch::async,[&]{return ordered.PushCommand(7,true,true);});
 for(int taken=0;taken<8;) {
   const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
   CommandDataContainer pending;
   while(!ordered.state.queue.TryPop(pending)) {
     assert(std::chrono::steady_clock::now()<deadline);
     std::this_thread::yield();
   }
   SignalCommand(ordered.state,pending.fence,pending.block);
   ++taken;
 }
 for(auto& wait:waits) assert(wait.wait_for(std::chrono::seconds(1))==std::future_status::ready);
 for(auto& wait:waits) assert(wait.get()<=8);
 // A stop requested before the push: nothing is queued and nothing waits.
 ThreadManager cancelled;
 cancelled.stop_source.request_stop();
 cancelled.PushCommand(8,true,true);
 assert(!cancelled.state.queue.TryPop(item));
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'check.cpp';p.write_text(code);exe=Path(tmp)/'check'
 subprocess.run(['c++','-std=c++20','-pthread','-fsanitize=address,undefined','-I'+str(cache/'source/src'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)
print('GPU producer: bounded queue order, full-queue and fence waits released by stop, signalled completion, ordered producers PASS')
