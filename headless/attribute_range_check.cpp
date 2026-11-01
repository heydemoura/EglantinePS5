// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include "shader_recompiler/frontend/ir/attribute.h"

int main() {
    using namespace Shader::IR;
    for (unsigned i = 0; i < 32 * 4; ++i) {
        const auto attribute = Attribute::Generic0X + i;
        assert(IsGeneric(attribute));
        assert(GenericAttributeIndex(attribute) == i / 4);
        assert(GenericAttributeElement(attribute) == i % 4);
    }
    assert(!IsGeneric(Attribute::PositionW));
    assert(!IsGeneric(Attribute::ColorFrontDiffuseR));
    assert(!IsGeneric(Attribute::ViewportMask));
}
