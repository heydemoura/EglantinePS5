// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Eden {
// ACI0 need not fit in the first KiB (Switch 2 dumps can place it at 0x540).
// Read only the header and the required part of ACI0, checking both short reads.
inline std::uint64_t ReadNpdmTitleId(std::FILE* file) {
    unsigned char header[0x80]{};
    if (!file || std::fseek(file, 0, SEEK_SET) != 0 ||
        std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header, "META", 4) != 0) return 0;
    std::uint32_t offset = 0;
    std::memcpy(&offset, header + 0x70, sizeof(offset));
    if (offset < sizeof(header) || std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0)
        return 0;
    unsigned char aci[0x18]{};
    if (std::fread(aci, 1, sizeof(aci), file) != sizeof(aci) ||
        std::memcmp(aci, "ACI0", 4) != 0) return 0;
    std::uint64_t title = 0;
    std::memcpy(&title, aci + 0x10, sizeof(title));
    return title;
}
}
