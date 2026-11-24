#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# From PS5_Lichii (mihawk, codeberg.org/mihawk/PS5_Lichii, commit 5a424f7), ps5/tools/texture-store/build.sh.
# Changed here: where the emulator's CityHash lives (Eglantine/src).
#
# Fetch the BC7/BC5 encoder sources (bc7enc_rdo, pinned) and build libbc7tex.so.
# Everything lands in $TEXTURE_STORE_CACHE (default ~/.cache/texture-store), outside any repository.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
cache=${TEXTURE_STORE_CACHE:-$HOME/.cache/texture-store}
rev=b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
sum=bbb33d1dbb6178a3a2c044956c3cc0b8ea9fc2f2903824379dd86f87d53b64c6
src=$cache/bc7enc_rdo-b943862
if [ ! -f "$src/bc7e.ispc" ]; then
    mkdir -p "$src"
    curl -sfL -o "$src/src.tar.gz" "https://github.com/richgel999/bc7enc_rdo/archive/$rev.tar.gz"
    echo "$sum  $src/src.tar.gz" | sha256sum -c -
    tar xzf "$src/src.tar.gz" -C "$src" --strip-components=1
    rm -f "$src/ispc.exe"   # a prebuilt Windows binary in the archive; never used
fi
mkdir -p "$cache/build"
ispc -O2 --wno-perf --target=avx2-i32x8 --opt=fast-math --opt=disable-assertions --pic \
    "$src/bc7e.ispc" -o "$cache/build/bc7e.o" -h "$cache/build/bc7e_ispc.h"
core=$(cd "$here/../../Eglantine/src" && pwd)
g++ -std=c++20 -O3 -march=native -shared -fPIC -I"$cache/build" -I"$src" -I"$core" \
    "$here/bc7tex.cpp" "$src/rgbcx.cpp" "$core/common/cityhash.cpp" "$cache/build/bc7e.o" -o "$cache/libbc7tex.so"
echo "built $cache/libbc7tex.so"
