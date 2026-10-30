// EglantinePS5 - Launcher colours, type sizes and motion constants.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pe/gfx/draw_list.hpp"

#include <algorithm>

namespace pe::ui
{

using gfx::Align;
using gfx::Color;
using gfx::Rect;

namespace theme
{

// Backgrounds
inline const Color kBase = Color::rgb(0x0a0606);
inline const Color kScrim = Color::rgb(0x080102);
inline const Color kGlass = Color::rgb(0x221618);  // home panels
inline const Color kPanel = Color::rgb(0x170b0d);  // screens and dialogs
inline const Color kPanelEdge = Color::rgb(0x8e757b);
inline const Color kRow = Color::rgb(0x241719);
inline const Color kRowEdge = Color::rgb(0x82676e);
inline const Color kRowFocus = Color::rgb(0x44292e);

// The accent: the rose's petal-tip pink to deep rose, left to right.
inline const Color kLime = Color::rgb(0xe23b60);
inline const Color kLimeDeep = Color::rgb(0x7a1f3d);
inline const Color kLimePale = Color::rgb(0xe8a6bc);
inline const Color kSun = Color::rgb(0xffb3c4);

// Text
inline const Color kText = Color::rgb(0xf5f0f1);
inline const Color kTitle = Color::rgb(0xfff2f5);
inline const Color kValue = Color::rgb(0xf4e9ed);
inline const Color kBody = Color::rgb(0xe8d7dc);
inline const Color kCopy = Color::rgb(0xd0bdc2);
inline const Color kMeta = Color::rgb(0xc6b5b9);
inline const Color kLabel = Color::rgb(0xbdaaaf);
inline const Color kMuted = Color::rgb(0xb8aaad);
inline const Color kFaint = Color::rgb(0x9e8c90);
inline const Color kWarning = Color::rgb(0xe8b39a);
inline const Color kRule = Color::rgb(0x6b5358);

// Type sizes (Montserrat Medium)
constexpr float kDisplay = 48.0f;
constexpr float kLead = 40.0f;
constexpr float kBrand = 36.0f;
constexpr float kHeading = 32.0f;
constexpr float kClock = 28.0f;
constexpr float kText24 = 24.0f;
constexpr float kSmall = 20.0f;

// Motion (spring responsiveness in rad/s, durations in seconds)
constexpr float kFocusSpring = 20.0f;
constexpr float kCursorSpring = 26.0f;
constexpr float kScrollSpring = 16.0f;
constexpr float kScreenSeconds = 0.30f;
constexpr float kLaunchSeconds = 0.95f;

} // namespace theme

// How the launcher is shown: Settings > Accessibility.
struct Look
{
    bool large_text = false;    // small text is drawn larger
    bool high_contrast = false; // solid panels, brighter text, an outlined highlight
    bool reduce_motion = false; // nothing drifts, slides or zooms: screens fade
};
inline Look &look()
{
    static Look value;
    return value;
}
// 1 normally, 0 with reduced motion: every slide, zoom and drift is multiplied by it.
inline float motion()
{
    return look().reduce_motion ? 0.0f : 1.0f;
}
// The size text of `size` is drawn at. "Larger text" raises everything under 30 to between 26
// and 30 (26 is the least the console accessibility guidelines ask of text on a 1080p TV).
inline float text_size(float size)
{
    return look().large_text && size < 30.0f ? std::clamp(size * 1.3f, 26.0f, 30.0f) : size;
}

// The baseline that centres text of `size` in a line box of height `line` starting at `top`.
inline float baseline(float top, float line, float size)
{
    return top + line * 0.5f + size * 0.35f;
}

inline bool inside(const Rect &r, float x, float y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

} // namespace pe::ui
