// SPDX-License-Identifier: GPL-3.0-or-later
#include "npdm_title_id.h"
#include <cassert>
#include <memory>
#include <vector>

static std::vector<unsigned char> Image(std::uint32_t offset, std::uint64_t title) {
    std::vector<unsigned char> bytes(offset + 0x18);
    std::memcpy(bytes.data(), "META", 4);
    std::memcpy(bytes.data() + 0x70, &offset, sizeof(offset));
    std::memcpy(bytes.data() + offset, "ACI0", 4);
    std::memcpy(bytes.data() + offset + 0x10, &title, sizeof(title));
    return bytes;
}
static std::uint64_t Read(const std::vector<unsigned char>& bytes) {
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::tmpfile(), &std::fclose);
    assert(file);
    assert(std::fwrite(bytes.data(), 1, bytes.size(), file.get()) == bytes.size());
    return Eden::ReadNpdmTitleId(file.get());
}
int main() {
    constexpr std::uint64_t title = 0x040074A01BF12000;
    for (const auto offset : {0x80u, 0x380u, 0x540u, 0x1000u})
        assert(Read(Image(offset, title)) == title);
    auto bytes = Image(0x540, title);
    bytes.pop_back();
    assert(Read(bytes) == 0);
    bytes = Image(0x540, title);
    bytes[0] = 0;
    assert(Read(bytes) == 0);
    bytes = Image(0x540, title);
    bytes[0x540] = 0;
    assert(Read(bytes) == 0);
    for (const std::uint32_t offset : {0u, 0x7fu, 0xfffffff0u}) {
        bytes = Image(0x80, title);
        std::memcpy(bytes.data() + 0x70, &offset, sizeof(offset));
        assert(Read(bytes) == 0);
    }
    assert(Read(std::vector<unsigned char>(0x7f)) == 0);
    assert(Eden::ReadNpdmTitleId(nullptr) == 0);
}
