// Standalone test of the production range tracker; no Vulkan or console required.
#include "video_core/readback_range_tracker.h"

#include <array>
#include <cassert>
#include <iostream>
#include <random>

using Tracker = VideoCommon::ReadbackRangeTracker;

int main() {
    Tracker tracker{1, 256};
    assert(!tracker.Lookup(0, 1));
    assert(tracker.RecordGpuWrite(0, 128, 7));
    const auto old = *tracker.Lookup(0, 64);
    assert(old.producer == 7);
    // Later disjoint submissions must not extend this range's dependency.
    assert(tracker.RecordGpuWrite(128, 128, 900));
    assert(tracker.IsCurrent(old));
    assert(tracker.Lookup(64, 128)->producer == 900);
    // Same-submission overwrite is still a different version.
    const auto right = *tracker.Lookup(128, 128);
    assert(tracker.RecordGpuWrite(128, 1, 900));
    assert(!tracker.IsCurrent(right));
    assert(tracker.IsCurrent(old));
    tracker.Invalidate(16, 1);
    assert(!tracker.IsCurrent(old));
    assert(!tracker.Lookup(0, 64));
    assert(tracker.Lookup(0, 16));
    assert(tracker.Lookup(17, 47));
    const auto before_remap = *tracker.Lookup(17, 47);
    tracker.InvalidateAll();
    assert(tracker.RecordGpuWrite(0, 256, 901));
    assert(!tracker.IsCurrent(before_remap));
    Tracker replacement{2, 256};
    assert(replacement.RecordGpuWrite(0, 256, 901));
    assert(!replacement.IsCurrent(*tracker.Lookup(0, 256)));
    assert(!tracker.RecordGpuWrite(0, 1, 900));
    assert(!tracker.Lookup(0, 256));
    assert(!tracker.RecordGpuWrite(255, 2, 902));
    assert(!tracker.RecordGpuWrite(~0ULL, 4, 902));
    assert(!tracker.RecordGpuWrite(0, 0, 902));
    assert(!tracker.RecordGpuWrite(0, 1, 0));
    assert(!tracker.Lookup(0, 0));
    assert(!tracker.Lookup(255, 2));
    Tracker bounded{3, 256, 2};
    assert(bounded.RecordGpuWrite(0, 256, 1));
    const auto before_overflow = *bounded.Lookup(0, 256);
    assert(!bounded.RecordGpuWrite(64, 1, 2));
    assert(bounded.SegmentCount() == 0);
    assert(!bounded.IsCurrent(before_overflow));
    assert(!bounded.Lookup(64, 1));
    assert(bounded.RecordGpuWrite(0, 256, 3));
    const auto retained = *bounded.Lookup(0, 256);
    Tracker relocated{std::move(bounded)};
    assert(relocated.IsCurrent(retained));
    assert(!bounded.IsCurrent(retained));
    assert(!bounded.RecordGpuWrite(0, 256, 4));
    const auto displaced = *replacement.Lookup(0, 256);
    replacement = std::move(relocated);
    assert(replacement.IsCurrent(retained));
    assert(!replacement.IsCurrent(displaced));
    assert(!relocated.IsCurrent(retained));

    // Independent byte oracle: check range coverage, latest producer, and stale
    // tickets after random partial overwrites, unknown writes and whole remaps.
    Tracker random{4, 256, 512};
    std::array<unsigned long long, 256> producers{}, revisions{};
    std::mt19937 rng{923174};
    for (unsigned iteration = 1; iteration <= 20000; ++iteration) {
        const auto start = rng() % 256;
        const auto length = 1 + rng() % (256 - start);
        const auto ticket = random.Lookup(start, length);
        const auto previous_revisions = revisions;
        const auto op = rng() % 10;
        const auto write_start = rng() % 256;
        const auto write_length = 1 + rng() % (256 - write_start);
        if (op == 0) {
            random.InvalidateAll();
            producers.fill(0);
            revisions.fill(iteration);
        } else {
            if (op < 3) {
                random.Invalidate(write_start, write_length);
            } else {
                assert(random.RecordGpuWrite(write_start, write_length, iteration));
            }
            for (auto i = write_start; i < write_start + write_length; ++i) {
                producers[i] = op < 3 ? 0 : iteration;
                revisions[i] = iteration;
            }
        }
        if (ticket) {
            bool unchanged = true;
            for (auto i = start; i < start + length; ++i) {
                unchanged &= previous_revisions[i] == revisions[i];
            }
            assert(random.IsCurrent(*ticket) == unchanged);
        }
        for (int query = 0; query < 12; ++query) {
            const auto begin = rng() % 256;
            const auto count = 1 + rng() % (256 - begin);
            bool known = true;
            unsigned long long latest = 0;
            for (auto i = begin; i < begin + count; ++i) {
                known &= producers[i] != 0;
                latest = std::max(latest, producers[i]);
            }
            const auto actual = random.Lookup(begin, count);
            assert(actual.has_value() == known);
            if (actual) {
                assert(actual->producer == latest);
                assert(random.IsCurrent(*actual));
            }
        }
    }
    std::cout << "PASS: 240000 oracle queries, version/producer separation, alias/remap "
                 "invalidation, lifetime identity, bounds and bounded fallback\n";
}
