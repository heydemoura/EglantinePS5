// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>

namespace Eden {
// Read the ACI0 block at the offset declared by META, including offsets beyond 1 KiB.
inline std::uint64_t ExtractedTitleID(const std::string& npdm) {
    std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(npdm.c_str(), "rb"), std::fclose);
    unsigned char header[0x80]{};
    if (!file || std::fread(header, 1, sizeof(header), file.get()) != sizeof(header) ||
        std::memcmp(header, "META", 4) != 0) return 0;
    std::uint32_t offset{};
    std::memcpy(&offset, header + 0x70, sizeof(offset));
    unsigned char aci[0x18]{};
    if (offset < sizeof(header) || std::fseek(file.get(), offset, SEEK_SET) != 0 ||
        std::fread(aci, 1, sizeof(aci), file.get()) != sizeof(aci) ||
        std::memcmp(aci, "ACI0", 4) != 0) return 0;
    std::uint64_t title{};
    std::memcpy(&title, aci + 0x10, sizeof(title));
    return title;
}

// Keep the library key at the game root; the loader combines versioned updates with base.
inline std::string ExtractedGameMain(const std::string& root) {
    for (const char* suffix : {"/base/exefs/main", "/exefs/main"}) {
        const std::string path = root + suffix;
        struct stat info {};
        if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) return path;
    }
    return {};
}
}
