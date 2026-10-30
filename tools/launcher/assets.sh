#!/usr/bin/env bash
# EglantinePS5 - Regenerate the launcher's baked assets (font atlas and art).
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The results are committed under headless/eglantineps5/ui; run this only after changing the
# font, the glyph set or the source images. Needs a host C++ compiler and Python with Pillow.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=${EGLANTINEPS5_LAUNCHER_BUILD:-"$HOME/.cache/eglantineps5-launcher"}
mkdir -p "$build" "$root/headless/eglantineps5/ui/fonts"

"${HOST_CXX:-c++}" -std=c++20 -O2 -w -I"$root/tools/launcher" -I"$root/headless/eglantineps5" \
    "$root/tools/launcher/bake_font.cpp" -o "$build/bake_font"
"$build/bake_font" "$root/third_party/fonts/Montserrat-Medium.ttf" \
    "$root/headless/eglantineps5/ui/fonts/montserrat-medium.pefont"
python3 "$root/tools/launcher/render-art.py"
