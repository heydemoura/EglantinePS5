// SPDX-License-Identifier: GPL-3.0-or-later
#include "headless/native_directory_metadata.h"
#include <cassert>
#include <fstream>
#include <map>
#include <sys/syscall.h>
static int malformed{}, stats{};
extern "C" int __real_stat(const char*, struct stat*);
extern "C" int __wrap_stat(const char* path, struct stat* out) { ++stats; return __real_stat(path,out); }
extern "C" int sceKernelGetdents(int fd, char* out, int size) {
    if (malformed) {
        std::memset(out, 0, size);
        if (malformed == 1) return 1;
        const std::uint16_t length = malformed == 2 ? 65535 : offsetof(dirent,d_name)+4;
        std::memcpy(out+offsetof(dirent,d_reclen), &length, sizeof(length));
        if (malformed == 3) std::memcpy(out+offsetof(dirent,d_name), "a/b", 4);
        if (malformed == 4) std::memset(out+offsetof(dirent,d_name), 'a', 4);
        return offsetof(dirent,d_name)+4;
    }
    return syscall(SYS_getdents64,fd,out,size);
}
extern "C" bool nfs_is_path(const char* path) { return std::string_view(path).find("/nfs-test") != std::string_view::npos; }
extern "C" int nfs_list_each(const char*, void(*cb)(void*,const char*,unsigned long long,int), void* ctx) {
    cb(ctx,"file",0,0); cb(ctx,"dir",0,1); cb(ctx,".",0,1); return 3;
}
int main() {
    namespace fs=std::filesystem;
    auto root=fs::temp_directory_path()/ ("eden-dir-"+std::to_string(getpid()));
    fs::create_directory(root);fs::create_directory(root/"dir");
    std::ofstream(root/"file") << "hello";std::ofstream(root/"empty");
    std::error_code error;
    stats=0;auto files=Eden::ReadNativeDirectoryMetadata(root,false,error);
    assert(!error && files.size()==2 && stats==2);
    for(const auto& f:files)assert(!f.directory && f.size==fs::file_size(f.path));
    stats=0;auto dirs=Eden::ReadNativeDirectoryMetadata(root,true,error);
    assert(!error && dirs.size()==1 && dirs[0].directory && stats==0);
    fs::create_symlink("file",root/"file-link");fs::create_directory_symlink("dir",root/"dir-link");
    files=Eden::ReadNativeDirectoryMetadata(root,false,error);assert(!error && files.size()==3);
    dirs=Eden::ReadNativeDirectoryMetadata(root,true,error);assert(!error && dirs.size()==2);
    std::ofstream(root/"file",std::ios::app)<<" changed";
    files=Eden::ReadNativeDirectoryMetadata(root,false,error);
    for(const auto& f:files)assert(f.size==fs::file_size(f.path));
    fs::create_symlink("missing",root/"broken");
    assert(Eden::ReadNativeDirectoryMetadata(root,false,error).empty() && error);
    fs::remove(root/"broken");
    for(malformed=1;malformed<=4;++malformed)assert(Eden::ReadNativeDirectoryMetadata(root,false,error).empty() && error);
    malformed=0;
    assert(Eden::ReadNativeDirectoryMetadata(root/"absent",false,error).empty() && error);
    fs::create_directory(root/"nfs-test"); fs::create_directory(root/"nfs-test/dir");
    std::ofstream(root/"nfs-test/file") << "hello";
    stats=0;files=Eden::ReadNativeDirectoryMetadata(root/"nfs-test",false,error);
    assert(!error && files.size()==1 && files[0].size==5 && stats==2);
    dirs=Eden::ReadNativeDirectoryMetadata(root/"nfs-test",true,error);
    assert(!error && dirs.size()==1 && dirs[0].directory && stats==4);
    fs::remove_all(root);
    std::puts("Native directory metadata PASS: one stat/file, zero stat/known directory, symlinks, changed sizes, errors, malformed records, NFS metadata");
}
