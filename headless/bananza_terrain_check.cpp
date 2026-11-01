// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include "video_core/bananza_terrain_compat.h"

int main() {
    Tegra::BananzaTerrainDrawCompat compat;
    assert(compat.RecordDepthDraw(1, 0x1000, 0x2000, 7500, 1));
    // Appending to the same counter in one pass must preserve earlier output.
    assert(!compat.RecordDepthDraw(1, 0x1000, 0x2000, 7500, 1));
    assert(compat.RecordDepthDraw(1, 0x3000, 0x4000, 300, 1));
    assert(compat.RecordDepthDraw(2, 0x1000, 0x2000, 7500, 1));
    assert(compat.RecordDepthDraw(1, 0x1000, 0x5000, 7500, 1));
    assert(!compat.RecordDepthDraw(1, 0x1000, 0x5000, 7500, 1));
    assert(!compat.FindInstances(1, 0x1000, 0x5000, 99));
    compat.BeginColorPass();
    assert(compat.FindInstances(1, 0x1000, 0x5000, 99) == 1);
    assert(!compat.FindInstances(1, 0x1000, 0x5000, 7503));
    assert(!compat.FindInstances(1, 0x1000, 0x5000, 100));
    assert(!compat.FindInstances(1, 0x1000, 0x2000, 99));
    // Reusing the same buffers in the next depth pass needs fresh arguments.
    assert(compat.RecordDepthDraw(1, 0x1000, 0x5000, 7500, 1));
    assert(!compat.RecordDepthDraw(1, 0x1000, 0x5000, 7500, 1));
    assert(!compat.RecordDepthDraw(1, 0x9000, 0xa000, 300, 0));
    assert(!compat.RecordDepthDraw(1, 0x9000, 0xa000, 300, 2));
    assert(!compat.RecordDepthDraw(1, 0x9000, 0xa000, 0, 1));
    assert(!compat.RecordDepthDraw(1, 0, 0xa000, 300, 1));
    assert(!compat.RecordDepthDraw(1, 0x9000, 0, 300, 1));
    assert(compat.RecordDepthDraw(1, 0x9000, 0xa000, 300, 1));
}
