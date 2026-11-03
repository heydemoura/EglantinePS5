// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_directory.h"
#include <sys/stat.h>

namespace Eden {
struct NativeDirectoryMetadata {
    std::filesystem::path path;
    std::uint64_t size{};
    bool directory{};
};

// Short-lived enumeration snapshot. Unlike constructing directory_entry and then
// calling status()/file_size(), this performs at most one followed stat per file.
// Nothing caches file contents or survives into the next directory enumeration.
inline std::vector<NativeDirectoryMetadata> ReadNativeDirectoryMetadata(
        const std::filesystem::path& path, bool directories, std::error_code& error) {
    std::vector<NativeDirectoryMetadata> entries;
    error.clear();
    if (nfs_is_path && nfs_is_path(path.c_str())) {
        // The shared API also serves NTFS, whose listings report size=0.
        // Resolve metadata once through the backend's stat wrapper; do not treat
        // a listing size or boolean directory hint as complete file metadata.
        struct Context {
            const std::filesystem::path& parent;
            std::vector<std::filesystem::path> paths;
            bool invalid{};
        } context{path, {}};
        const int count = nfs_list_each(path.c_str(), [](void* user, const char* name,
                                                      unsigned long long, int) {
            auto& c = *static_cast<Context*>(user);
            const std::string_view filename{name};
            if (filename == "." || filename == "..") return;
            if (filename.empty() || filename.find('/') != filename.npos) {
                c.invalid = true;
                return;
            }
            c.paths.push_back(c.parent / filename);
        }, &context);
        if (count < 0 || context.invalid) {
            error = count < 0 ? std::error_code{-count, std::generic_category()}
                              : std::make_error_code(std::errc::io_error);
            return {};
        }
        // Outside the listing callback: backend operations may share a mutex.
        for (const auto& full_path : context.paths) {
            struct stat status{};
            if (::stat(full_path.c_str(), &status) != 0) {
                error = {errno, std::generic_category()}; return {};
            }
            if (directories ? S_ISDIR(status.st_mode) : S_ISREG(status.st_mode))
                entries.push_back({full_path, static_cast<std::uint64_t>(status.st_size), directories});
        }
        return entries;
    }
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) { error = {errno, std::generic_category()}; return {}; }
    struct Close { int fd; ~Close() { close(fd); } } close_fd{fd};
    std::vector<char> buffer(65536);
    for (;;) {
        const int count = sceKernelGetdents(fd, buffer.data(), buffer.size());
        if (!count) return entries;
        if (count < 0 || count > static_cast<int>(buffer.size())) {
            error = std::make_error_code(std::errc::io_error); return {};
        }
        std::size_t offset{};
        while (offset < static_cast<std::size_t>(count)) {
            constexpr auto name_offset = offsetof(dirent, d_name);
            const auto available = static_cast<std::size_t>(count) - offset;
            if (available <= name_offset) {
                error = std::make_error_code(std::errc::io_error); return {};
            }
            std::uint16_t length;
            std::memcpy(&length, buffer.data() + offset + offsetof(dirent, d_reclen), sizeof(length));
            if (length <= name_offset || length > available) {
                error = std::make_error_code(std::errc::io_error); return {};
            }
            const char* name = buffer.data() + offset + name_offset;
            const char* end = static_cast<const char*>(std::memchr(name, 0, length - name_offset));
            if (!end || end == name || std::memchr(name, '/', end - name)) {
                error = std::make_error_code(std::errc::io_error); return {};
            }
            const std::string_view filename{name, static_cast<std::size_t>(end - name)};
            unsigned char type;
            std::memcpy(&type, buffer.data() + offset + offsetof(dirent, d_type), sizeof(type));
            offset += length;
            if (filename == "." || filename == "..") continue;
            if (directories && type == DT_REG) continue;
            if (!directories && type == DT_DIR) continue;
            if (type != DT_REG && type != DT_DIR && type != DT_LNK && type != DT_UNKNOWN) continue;
            const auto full_path = path / filename;
            if (directories && type == DT_DIR) {
                entries.push_back({full_path, 0, true});
                continue;
            }
            // Follow symlinks, including unknown d_type filesystems, as the old
            // VFS status() filter did. Preserve errors instead of hiding entries.
            struct stat status{};
            if (::stat(full_path.c_str(), &status) != 0) {
                error = {errno, std::generic_category()}; return {};
            }
            if (directories ? S_ISDIR(status.st_mode) : S_ISREG(status.st_mode))
                entries.push_back({full_path, static_cast<std::uint64_t>(status.st_size), directories});
        }
    }
}
} // namespace Eden
