#include "video_core/readback_write_tracker.h"
#include <cassert>
#include <iostream>
#include <thread>
using Tracker = VideoCommon::ReadbackWriteTracker<unsigned long long>;
int main() {
    Tracker disabled;
    assert(!disabled.Register(1, 1024, false));
    Tracker registry{true};
    assert(!registry.Register(1, 1024, true));
    assert(!registry.Register(1, 512*1024+1, false));
    auto a = registry.Register(1, 1024, false);
    auto b = registry.Register(2, 1024, false);
    assert(a && b && a->identity != b->identity);
    registry.TransferWrite(1, 0, 1024, 1, false);
    const auto initial = *a->Lookup(0, 1024);
    registry.BeginBatch();
    registry.NoteShaderWrite(1, 0, 64);
    assert(!a->Lookup(0, 64));
    // A transfer during descriptor configuration cannot expose a pending shader write.
    registry.TransferWrite(1, 0, 64, 2, false);
    assert(!a->Lookup(0, 64));
    registry.CompleteBatch(3);
    assert(a->Lookup(0, 64)->producer == 3);
    assert(a->Lookup(64, 960)->producer == 1);
    assert(!a->ranges.IsCurrent(initial));
    registry.TransferWrite(2, 0, 1024, 900, false);
    assert(a->Lookup(0, 64)->producer == 3);
    registry.BeginBatch();
    registry.NoteShaderWrite(1, 128, 64);
    registry.TransferWrite(1, 128, 64, 4, false);
    registry.AbortBatch();
    assert(!a->Lookup(128, 64));
    assert(a->Lookup(0, 64)->producer == 3);
    registry.TransferWrite(1, 0, 1, 5, true);
    assert(!a->Lookup(0, 64));
    // Pending records retain tracker identity, not a reused handle's new state.
    registry.BeginBatch();
    registry.NoteShaderWrite(1, 0, 64);
    auto replacement = registry.Register(1, 1024, false);
    assert(replacement->identity != a->identity);
    registry.CompleteBatch(6);
    assert(a->Lookup(0, 64)->producer == 6);
    assert(!replacement->Lookup(0, 64));
    registry.TransferWrite(1, 0, 64, 7, false);
    registry.InvalidateAll();
    assert(!replacement->Lookup(0, 64));
    assert(!b->Lookup(0, 1024));
    // Exhausted coverage disables the diagnostic and invalidates every certificate.
    registry.TransferWrite(1, 0, 1024, 8, false);
    registry.BeginBatch();
    for (unsigned i = 0; i < 1025; ++i) registry.NoteShaderWrite(1, 0, 1);
    assert(!registry.Enabled());
    registry.TransferWrite(1, 0, 1024, 9, false);
    registry.CompleteBatch(10);
    assert(!replacement->Lookup(0, 1024));
    assert(replacement->pending == 0);
    // Mapping notifications only touch the atomic epoch while the owner updates ranges.
    Tracker remaps{true};
    auto mapped = remaps.Register(1, 1024, false);
    std::thread notifier([&] { for (unsigned i = 0; i < 10000; ++i) remaps.InvalidateAll(); });
    for (unsigned i = 1; i <= 10000; ++i) {
        remaps.TransferWrite(1, 0, 1024, i, false);
        (void)mapped->Lookup(0, 1024);
    }
    notifier.join();
    remaps.TransferWrite(1, 0, 1024, 10001, false);
    assert(mapped->Lookup(0, 1024)->producer == 10001);
    remaps.InvalidateAll();
    assert(!mapped->Lookup(0, 1024));
    // Expired allocations are pruned without exhausting the registry indefinitely.
    Tracker lifetimes{true};
    for (unsigned i = 0; i < 10000; ++i) {
        auto temporary = lifetimes.Register(i, 1024, false);
        assert(temporary);
    }
    std::vector<std::shared_ptr<Tracker::State>> live;
    for (unsigned i = 0; i < 256; ++i) live.push_back(lifetimes.Register(i,1024,false));
    assert(live.back());
    assert(!lifetimes.Register(1000, 1024, false));
    std::cout << "PASS: writer submission timing, pending/aborted shaders, reordered upload, "
                 "handle reuse, bounded coverage failure and 10000 allocation lifetimes\n";
}
