// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "common/settings.h"
#include "video_core/renderer_vulkan/present/frame_gen_pacer.h"

namespace Vulkan {

namespace {

using Clock = std::chrono::steady_clock;

constexpr f32 INTERVAL_SMOOTHING = 0.25f;
constexpr f32 MINIMUM_BASE_RATE = 10.0f;
constexpr f32 BURST_CADENCE_RATIO = 3.0f;
constexpr f32 BURST_TARGET_RATIO = 2.0f;
constexpr f32 PROBE_THROUGHPUT_TOLERANCE = 0.95f;
constexpr f32 PROBE_BASE_COLLAPSE_RATIO = 0.70f;
constexpr f32 PROBE_MARGINAL_GAIN = 1.15f;
constexpr f32 TARGET_SATISFIED_RATIO = 0.95f;
constexpr f32 UNLOADED_BASE_RETENTION = 0.75f;
constexpr f32 CREDIT_EPSILON = 1.0e-4f;
constexpr u32 MAX_PROBE_FAILURES = 4;
/// Outputs per real frame for a wanted (fractional) count: 2x from 1.3 (FIFO slows a faster
/// guest to half the refresh, which is smooth), more only from k + 0.85, since each step past
/// 2x slows the guest to refresh/k (3x: 20 fps at 60 Hz).
[[nodiscard]] size_t PickOutputs(f32 wanted) {
    if (wanted < 1.3f) {
        return 1;
    }
    return std::max<size_t>(2, static_cast<size_t>(std::floor(wanted + 0.15f)));
}
constexpr auto STEP_UP_DELAY = std::chrono::seconds(1);
constexpr auto DOWN_TRIAL_PERIOD = std::chrono::seconds(3);
constexpr auto DOWN_TRIAL_BACKOFF = std::chrono::seconds(10);
constexpr auto DOWN_TRIAL_DURATION = std::chrono::seconds(1);

constexpr auto STABILIZATION_DURATION = std::chrono::seconds(1);
constexpr auto PROBE_DURATION = std::chrono::seconds(1);
constexpr auto DEFICIT_DURATION = std::chrono::seconds(1);
constexpr auto PROBE_STEP_DELAY = std::chrono::milliseconds(250);

[[nodiscard]] Clock::duration ProbeBackoff(u32 failures) {
    switch (failures) {
    case 1:
        return std::chrono::seconds(5);
    case 2:
        return std::chrono::seconds(15);
    case 3:
        return std::chrono::seconds(30);
    default:
        return std::chrono::seconds(60);
    }
}

/// EDEN_FG_DITHER=1: the old per-frame output credit (varying generations per real frame).
[[nodiscard]] bool DitherOutputs() {
    static const bool dither = [] {
        const char* value = std::getenv("EDEN_FG_DITHER");
        return value != nullptr && value[0] == '1';
    }();
    return dither;
}

} // Anonymous namespace

FrameGenPlan FrameGenPacer::Plan(size_t capacity) {
    const size_t ceiling = std::min(capacity, Settings::FrameGenMaxGenerations());
    if (ceiling == 0) {
        Reset();
        return {};
    }

    const Clock::time_point now = Clock::now();
    const size_t previous_generations = std::exchange(issued_generations, 0);
    if (!last_frame) {
        last_frame = now;
        return {};
    }

    const Clock::duration interval = now - *last_frame;
    const f32 interval_seconds = std::chrono::duration<f32>(interval).count();
    last_frame = now;

    if (interval_seconds <= 0.0f) {
        Stabilize(now);
        return {};
    }

    const f32 target_rate = static_cast<f32>(Settings::values.frame_gen_target_rate.GetValue());

    if (smoothed_interval > 0.0f) {
        f32 burst_threshold = BURST_CADENCE_RATIO / smoothed_interval;
        if (target_rate > 0.0f) {
            burst_threshold = std::max(burst_threshold, target_rate * BURST_TARGET_RATIO);
        }
        if (1.0f / interval_seconds > burst_threshold) {
            DeferEvaluations(interval);
            output_credit = 0.0f;
            return {};
        }
    }

    if (interval_seconds > 1.0f / MINIMUM_BASE_RATE) {
        Stabilize(now);
        return {};
    }

    smoothed_interval = smoothed_interval > 0.0f
                            ? smoothed_interval +
                                  (interval_seconds - smoothed_interval) * INTERVAL_SMOOTHING
                            : interval_seconds;

    if (previous_generations == 0) {
        const f32 measured = 1.0f / smoothed_interval;
        unloaded_base_rate =
            unloaded_base_rate > 0.0f
                ? unloaded_base_rate + (measured - unloaded_base_rate) * INTERVAL_SMOOTHING
                : measured;
    }

    if (stable_until) {
        if (now < *stable_until) {
            return {};
        }
        stable_until.reset();
    }

    if (target_rate == 0.0f) {
        limit = std::min(Settings::FrameGenGenerations(), ceiling);
        output_credit = 0.0f;
        issued_generations = limit;
        return {.generations = limit, .warm = limit > 0};
    }

    UpdateLimit(now, 1.0f / smoothed_interval, target_rate, ceiling);

    const size_t allowed = std::min(limit, ceiling);
    const f32 desired_outputs = smoothed_interval * target_rate;
    if (allowed == 0 || desired_outputs <= 1.0f) {
        output_credit = 0.0f;
        return {};
    }

    if (!DitherOutputs()) {
        // One output count for every real frame. Varying it frame to frame (the credit below)
        // alternates half-interval and whole-interval motion steps: at 34 fps toward 60 every
        // fourth frame got no generated frame, a judder well visible in Zelda on BC250.
        // Frame generation presents FIFO, so a held count above what the guest needs only
        // slows the guest to refresh/count. Hence PickOutputs prefers 2x, a higher count needs a
        // full second of clear shortfall, and past 2x one step lower is tried now and then: a
        // FIFO-paced guest cannot show that it would run faster (Zelda walking: 3x held the
        // guest at 20 fps where 2x ran 23-29).
        const size_t wanted = PickOutputs(desired_outputs);
        if (down_trial_until) {
            if (now >= *down_trial_until) {
                down_trial_until.reset();
                if (wanted > held_outputs) {
                    ++held_outputs;
                    next_down_trial = now + DOWN_TRIAL_BACKOFF;
                } else {
                    next_down_trial = now + DOWN_TRIAL_PERIOD;
                }
            }
        } else if (held_outputs == 0 || wanted < held_outputs) {
            held_outputs = wanted;
            step_up_since.reset();
            next_down_trial = now + DOWN_TRIAL_PERIOD;
        } else if (wanted > held_outputs) {
            if (!step_up_since) {
                step_up_since = now;
            } else if (now - *step_up_since >= STEP_UP_DELAY) {
                ++held_outputs;
                step_up_since.reset();
                next_down_trial = now + DOWN_TRIAL_PERIOD;
            }
        } else {
            step_up_since.reset();
            if (held_outputs > 2 && next_down_trial && now >= *next_down_trial) {
                --held_outputs;
                down_trial_until = now + DOWN_TRIAL_DURATION;
            }
        }
        output_credit = 0.0f;
        const size_t held_generations = std::min(held_outputs - 1, allowed);
        issued_generations = held_generations;
        return {.generations = held_generations, .warm = true};
    }

    output_credit += desired_outputs;
    const size_t outputs =
        std::max<size_t>(1, static_cast<size_t>(std::floor(output_credit + CREDIT_EPSILON)));
    const size_t generations = std::min(outputs - 1, allowed);

    output_credit -= static_cast<f32>(generations + 1);
    if (output_credit < 0.0f) {
        output_credit = 0.0f;
    } else if (generations == allowed && output_credit >= 1.0f) {
        output_credit = std::fmod(output_credit, 1.0f);
    }

    issued_generations = generations;
    return {.generations = generations, .warm = true};
}

void FrameGenPacer::UpdateLimit(Clock::time_point now, f32 base_rate, f32 target_rate,
                                size_t ceiling) {
    limit = std::min(limit, ceiling);

    if (probe_until) {
        if (now < *probe_until) {
            return;
        }
        probe_until.reset();
        output_credit = 0.0f;

        const f32 previous_output =
            std::min(target_rate, probe_base_rate * static_cast<f32>(probe_previous_limit + 1));
        const f32 current_output =
            std::min(target_rate, base_rate * static_cast<f32>(limit + 1));

        const bool throughput_regressed =
            current_output < previous_output * PROBE_THROUGHPUT_TOLERANCE;
        const bool collapsed_for_marginal_gain =
            base_rate < probe_base_rate * PROBE_BASE_COLLAPSE_RATIO &&
            current_output < previous_output * PROBE_MARGINAL_GAIN;
        const bool emulation_slowed = unloaded_base_rate > 0.0f &&
                                      base_rate < unloaded_base_rate * UNLOADED_BASE_RETENTION;

        if (throughput_regressed || collapsed_for_marginal_gain || emulation_slowed) {
            limit = probe_previous_limit;
            probe_failures = std::min(probe_failures + 1, MAX_PROBE_FAILURES);
            next_probe = now + ProbeBackoff(probe_failures);
            deficit_since.reset();
            return;
        }

        probe_failures = 0;
        next_probe = now + PROBE_STEP_DELAY;
    }

    if (base_rate * static_cast<f32>(limit + 1) >= target_rate * TARGET_SATISFIED_RATIO ||
        limit >= ceiling) {
        deficit_since.reset();
        return;
    }

    if (!deficit_since) {
        deficit_since = now;
        return;
    }
    if (now - *deficit_since < DEFICIT_DURATION) {
        return;
    }
    if (next_probe && now < *next_probe) {
        return;
    }

    probe_previous_limit = limit;
    probe_base_rate = base_rate;
    ++limit;
    probe_until = now + PROBE_DURATION;
    deficit_since.reset();
    output_credit = 0.0f;
}

void FrameGenPacer::DeferEvaluations(Clock::duration amount) {
    const auto defer = [amount](std::optional<Clock::time_point>& deadline) {
        if (deadline) {
            *deadline += amount;
        }
    };
    defer(stable_until);
    defer(probe_until);
    defer(next_probe);
    deficit_since.reset();
}

void FrameGenPacer::Stabilize(Clock::time_point now) {
    stable_until = now + STABILIZATION_DURATION;
    probe_until.reset();
    deficit_since.reset();
    smoothed_interval = 0.0f;
    output_credit = 0.0f;
    held_outputs = 0;
    down_trial_until.reset();
    next_down_trial.reset();
    step_up_since.reset();
}

void FrameGenPacer::Reset() {
    last_frame.reset();
    stable_until.reset();
    probe_until.reset();
    next_probe.reset();
    deficit_since.reset();
    smoothed_interval = 0.0f;
    output_credit = 0.0f;
    probe_base_rate = 0.0f;
    unloaded_base_rate = 0.0f;
    issued_generations = 0;
    probe_previous_limit = 0;
    limit = 0;
    probe_failures = 0;
    held_outputs = 0;
    down_trial_until.reset();
    next_down_trial.reset();
    step_up_since.reset();
}

} // namespace Vulkan
