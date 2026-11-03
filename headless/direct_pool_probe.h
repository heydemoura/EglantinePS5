// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <initializer_list>

namespace Eden::DirectPoolProbe {
struct Region { std::int64_t start{}, end{}; std::int32_t type{}; };
struct Result {
    std::uint64_t occupied{}, verified_free{}, fingerprint{1469598103934665603ULL};
    std::size_t regions{};
    int query_status{}, tail_status{}, gap_status{};
    bool complete{}, limited{}, invalid{}, unverified_gap{};
    std::optional<std::uint64_t> Free(std::uint64_t total) const {
        if (!complete || occupied > total || verified_free != total - occupied) return {};
        return verified_free;
    }
};
// Query lists allocated extents. A failed next-region query is NOT assumed to
// mean end-of-list: the independent free-block query must prove the whole tail
// is free. Time/cap limits and malformed/extending/overlapping extents fail closed.
// Enumeration is not atomic; callers compare two scans before trusting an estimate.
template <typename Query, typename TailFree, typename Continue>
Result Scan(std::int64_t total, std::size_t cap, Query&& query,
            TailFree&& tail_free, Continue&& proceed) {
    Result result;
    if (total <= 0) { result.invalid = true; return result; }
    std::int64_t cursor{};
    while (cursor < total) {
        if (result.regions == cap || !proceed()) { result.limited = true; return result; }
        Region region;
        result.query_status = query(cursor, region);
        if (result.query_status) {
            result.complete = tail_free(cursor, total, result.tail_status);
            if (result.complete) result.verified_free += static_cast<std::uint64_t>(total - cursor);
            return result;
        }
        if (region.start < cursor || region.end <= region.start || region.end > total || region.type < 0) {
            result.invalid = true;
            return result;
        }
        // Also verify interior gaps. This rejects a process-scoped or incomplete
        // region view if another allocation is hidden inside a claimed free gap.
        if (region.start > cursor) {
            if (!tail_free(cursor, region.start, result.gap_status)) {
                result.unverified_gap = true;
                return result;
            }
            result.verified_free += static_cast<std::uint64_t>(region.start - cursor);
        }
        result.occupied += static_cast<std::uint64_t>(region.end - region.start);
        for (const auto value : {static_cast<std::uint64_t>(region.start),
                                 static_cast<std::uint64_t>(region.end),
                                 static_cast<std::uint64_t>(region.type)}) {
            result.fingerprint ^= value;
            result.fingerprint *= 1099511628211ULL;
        }
        cursor = region.end;
        ++result.regions;
    }
    result.complete = true;
    return result;
}
inline bool Stable(const Result& a, const Result& b) {
    return a.complete && b.complete && a.regions == b.regions &&
           a.occupied == b.occupied && a.fingerprint == b.fingerprint;
}
} // namespace Eden::DirectPoolProbe
